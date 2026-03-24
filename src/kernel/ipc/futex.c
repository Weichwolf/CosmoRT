/* CosmoRT Futex — Fast Userspace Mutex with Priority Inheritance
 *
 * Spin-yield implementation: threads spin with PAUSE instead of true blocking.
 * Functionally correct for CosmoPX pthread_mutex — the fast path (uncontended)
 * never enters the kernel. Only contended locks hit this code, and they're rare.
 *
 * PI (Priority Inheritance): when a high-priority thread blocks on a PI futex
 * owned by a low-priority thread, the owner is temporarily boosted. This prevents
 * priority inversion per IEEE 1003.1b PTHREAD_PRIO_INHERIT.
 */

#include "futex.h"
#include "syscall.h"
#include "thread.h"
#include "process.h"
#include "percpu.h"
#include "spinlock.h"
#include "slab.h"
#include "serial.h"
#include "config.h"
#include "arch_x86.h"

extern void sched_add(thread_t *t);

/* ── Wait queue ─────────────────────────────────── */

#define FUTEX_HASH_SIZE  64
#define FUTEX_WAITER_MAX 256

typedef struct futex_waiter {
    thread_t             *thread;
    uint64_t              uaddr;   /* virtual address */
    uint32_t              pid;     /* process id — same vaddr in different processes */
    struct futex_waiter  *next;
} futex_waiter_t;

static futex_waiter_t waiter_pool[FUTEX_WAITER_MAX];
static slab_t waiter_slab;

static struct {
    futex_waiter_t *head;
    spinlock_t      lock;
} futex_hash[FUTEX_HASH_SIZE];

void futex_init(void) {
    slab_init(&waiter_slab, waiter_pool,
              (int)sizeof(futex_waiter_t), FUTEX_WAITER_MAX);
    for (int i = 0; i < FUTEX_HASH_SIZE; i++) {
        futex_hash[i].head = 0;
        futex_hash[i].lock = (spinlock_t)SPINLOCK_INIT;
    }
    serial_puts("futex: init (");
    char buf[4]; int bi = 0, v = FUTEX_HASH_SIZE;
    do { buf[bi++] = '0' + v % 10; v /= 10; } while (v);
    while (bi--) serial_putchar(buf[bi]);
    serial_puts(" buckets, ");
    bi = 0; v = FUTEX_WAITER_MAX;
    do { buf[bi++] = '0' + v % 10; v /= 10; } while (v);
    while (bi--) serial_putchar(buf[bi]);
    serial_puts(" waiters)\n");
}

static int hash_uaddr(uint64_t uaddr, uint32_t pid) {
    uint64_t h = uaddr ^ ((uint64_t)pid * 2654435761ULL);
    return (int)(h % FUTEX_HASH_SIZE);
}

/* ── Helpers ────────────────────────────────────── */

/* Boost owner's priority to at least `prio`.
 * Must be called with the futex bucket lock held — this serializes
 * priority modifications against concurrent PI operations. */
static void pi_boost(thread_t *owner, int prio) {
    if (!owner) return;
    if (owner->priority >= prio) return;

    if (owner->saved_priority < 0)
        owner->saved_priority = owner->priority;

    __sync_synchronize();
    owner->priority = prio;
    __sync_synchronize();
    /* Re-insertion into scheduler at new priority would require removing
     * from the old queue first. For spin-yield semantics the owner is
     * either RUNNING (and will be re-queued at the boosted priority on
     * next preemption) or RUNNABLE in a queue. The latter case is
     * imprecise but safe — it will pick up the new priority on its next
     * scheduling cycle. True queue re-insertion comes with true blocking. */
}

/* Restore owner's original priority after PI unlock. */
static void pi_unboost(thread_t *owner) {
    if (!owner) return;
    if (owner->saved_priority < 0) return;

    owner->priority = owner->saved_priority;
    owner->saved_priority = -1;
}

/* ── FUTEX_WAIT ─────────────────────────────────── */

extern uint64_t pml4[];

static long futex_wait(uint32_t *uaddr, uint32_t val) {
    /* Atomic check: if *uaddr != val, the wake already happened */
    if (__sync_val_compare_and_swap(uaddr, val, val) != val)
        return -EAGAIN;

    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    process_t *p = t->proc;
    uint32_t pid = p ? p->pid : 0;

    /* Add to wait queue under lock */
    int bucket = hash_uaddr((uint64_t)(uintptr_t)uaddr, pid);
    uint64_t flags;
    spin_lock_irq(&futex_hash[bucket].lock, &flags);

    /* Re-check under lock — value may have changed */
    if (__sync_val_compare_and_swap(uaddr, val, val) != val) {
        spin_unlock_irq(&futex_hash[bucket].lock, flags);
        return -EAGAIN;
    }

    /* Allocate waiter entry */
    futex_waiter_t *w = (futex_waiter_t *)slab_alloc(&waiter_slab);
    if (!w) {
        spin_unlock_irq(&futex_hash[bucket].lock, flags);
        return -ENOMEM;
    }
    w->thread = t;
    w->uaddr = (uint64_t)(uintptr_t)uaddr;
    w->pid = pid;
    w->next = futex_hash[bucket].head;
    futex_hash[bucket].head = w;

    spin_unlock_irq(&futex_hash[bucket].lock, flags);

    /* Save user state — when woken, thread resumes with return value 0 */
    save_user_state_for_block(t, 0);

    /* Block the thread */
    t->state = THREAD_BLOCKED;

    /* Switch to kernel page tables and return to scheduler */
    arch_set_cr3(virt_to_phys(pml4));
    thread_return_to_kernel(t);

    /* Unreachable — thread resumes via proc_enter_ring3 when woken */
    return 0;
}

/* ── FUTEX_WAKE ─────────────────────────────────── */

static long futex_wake(uint32_t *uaddr, uint32_t max_wake) {
    /* With spin-yield, waiters observe *uaddr directly.
     * The wake is implicit — changing *uaddr is sufficient.
     * We still walk the wait queue in case anyone registered there
     * (future true-blocking path). For now, return 0 (woke nobody
     * explicitly, but spin-waiters will see the change). */
    process_t *p = proc_current();
    uint32_t pid = p ? p->pid : 0;
    int bucket = hash_uaddr((uint64_t)(uintptr_t)uaddr, pid);
    long woken = 0;

    uint64_t flags;
    spin_lock_irq(&futex_hash[bucket].lock, &flags);

    futex_waiter_t **pp = &futex_hash[bucket].head;
    while (*pp && (uint32_t)woken < max_wake) {
        futex_waiter_t *w = *pp;
        if (w->uaddr == (uint64_t)(uintptr_t)uaddr && w->pid == pid) {
            /* Wake this thread */
            if (w->thread->state == THREAD_BLOCKED) {
                w->thread->rax = 0; /* return 0 from futex */
                sched_add(w->thread);
                woken++;
            }
            *pp = w->next;
            slab_free(&waiter_slab, w);
        } else {
            pp = &(*pp)->next;
        }
    }

    spin_unlock_irq(&futex_hash[bucket].lock, flags);
    return woken;
}

/* ── FUTEX_LOCK_PI ──────────────────────────────── */

static long futex_lock_pi(uint32_t *uaddr) {
    thread_t *self = thread_current();
    if (!self) return -EFAULT;
    uint32_t tid = (uint32_t)self->tid;

    /* Fast path: uncontended — CAS 0 → our TID */
    uint32_t old = __sync_val_compare_and_swap(uaddr, 0, tid);
    if (old == 0)
        return 0; /* acquired */

    /* Contended: boost owner, then spin-yield */
    uint32_t owner_tid = old & FUTEX_TID_MASK;
    thread_t *owner = thread_find_by_tid((int)owner_tid);
    pi_boost(owner, self->priority);

    /* Set FUTEX_WAITERS bit so unlock knows to check the queue */
    __sync_fetch_and_or(uaddr, FUTEX_WAITERS);

    /* Block via wait queue (no spin — strict spec compliance) */
    {
        process_t *p = self->proc;
        uint32_t pid = p ? p->pid : 0;
        int bucket = hash_uaddr((uint64_t)(uintptr_t)uaddr, pid);
        uint64_t flags;
        spin_lock_irq(&futex_hash[bucket].lock, &flags);

        /* Re-check before blocking — lock may have been released */
        old = __sync_val_compare_and_swap(uaddr, 0, tid);
        if (old == 0) {
            spin_unlock_irq(&futex_hash[bucket].lock, flags);
            return 0;
        }
        old = __sync_val_compare_and_swap(uaddr, FUTEX_WAITERS, tid | FUTEX_WAITERS);
        if (old == FUTEX_WAITERS) {
            spin_unlock_irq(&futex_hash[bucket].lock, flags);
            return 0;
        }

        futex_waiter_t *w = (futex_waiter_t *)slab_alloc(&waiter_slab);
        if (!w) {
            spin_unlock_irq(&futex_hash[bucket].lock, flags);
            return -ENOMEM;
        }
        w->thread = self;
        w->uaddr = (uint64_t)(uintptr_t)uaddr;
        w->pid = pid;
        w->next = futex_hash[bucket].head;
        futex_hash[bucket].head = w;

        spin_unlock_irq(&futex_hash[bucket].lock, flags);

        save_user_state_for_block(self, 0);
        self->state = THREAD_BLOCKED;
        arch_set_cr3(virt_to_phys(pml4));
        thread_return_to_kernel(self);
    }

    return 0;
}

/* ── FUTEX_UNLOCK_PI ────────────────────────────── */

static long futex_unlock_pi(uint32_t *uaddr) {
    thread_t *self = thread_current();
    if (!self) return -EFAULT;
    uint32_t tid = (uint32_t)self->tid;

    /* Verify we own this futex */
    uint32_t cur = __sync_val_compare_and_swap(uaddr, tid, tid);
    if ((cur & FUTEX_TID_MASK) != tid)
        return -EPERM;

    /* Restore our priority */
    pi_unboost(self);

    /* Release: clear our TID. Keep FUTEX_WAITERS if set so waiters can acquire. */
    if (cur & FUTEX_WAITERS) {
        /* Clear TID but keep WAITERS bit — a waiter will CAS it */
        __sync_val_compare_and_swap(uaddr, cur, FUTEX_WAITERS);
    } else {
        /* No waiters — just release */
        __sync_val_compare_and_swap(uaddr, cur, 0);
    }

    return 0;
}

/* ── Dispatcher ─────────────────────────────────── */

long do_futex(uint32_t *uaddr, int op, uint32_t val,
              const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3) {
    (void)timeout; /* timeout not implemented yet */
    (void)uaddr2;
    (void)val3;

    int cmd = op & ~(FUTEX_PRIVATE_FLAG | 0x100); /* strip PRIVATE + CLOCK_REALTIME */

    switch (cmd) {
    case FUTEX_WAIT:      return futex_wait(uaddr, val);
    case FUTEX_WAKE:      return futex_wake(uaddr, val);
    case FUTEX_LOCK_PI:   return futex_lock_pi(uaddr);
    case FUTEX_UNLOCK_PI: return futex_unlock_pi(uaddr);
    case FUTEX_WAKE_OP: {
        /* Simplified WAKE_OP: wake val waiters on uaddr, then
         * apply operation on *uaddr2 and conditionally wake val3
         * waiters on uaddr2.  We simplify: wake val on uaddr +
         * wake val3 on uaddr2 (skip the atomic op comparison). */
        long r1 = futex_wake(uaddr, val);
        if (uaddr2) {
            long r2 = futex_wake(uaddr2, val3);
            if (r2 > 0 && r1 >= 0) r1 += r2;
        }
        return r1;
    }
    case FUTEX_CMP_REQUEUE:
        /* Simplified: wake val waiters on uaddr, ignore requeue */
        return futex_wake(uaddr, val);
    case 9: /* FUTEX_WAIT_BITSET — treat as FUTEX_WAIT (ignore bitmask) */
        return futex_wait(uaddr, val);
    case 10: /* FUTEX_WAKE_BITSET — treat as FUTEX_WAKE */
        return futex_wake(uaddr, val);
    default:
        serial_puts("futex: unknown op ");
        serial_hex64((uint64_t)(unsigned)cmd);
        serial_putchar('\n');
        return -ENOSYS;
    }
}
