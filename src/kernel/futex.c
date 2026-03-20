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

static thread_t *find_thread_by_tid(int tid) {
    for (int i = 0; i < THREAD_MAX; i++) {
        if (thread_pool[i].state != THREAD_FREE &&
            thread_pool[i].state != THREAD_DEAD &&
            thread_pool[i].tid == tid)
            return &thread_pool[i];
    }
    return 0;
}

/* Boost owner's priority to at least `prio`. */
static void pi_boost(thread_t *owner, int prio) {
    if (!owner) return;
    if (owner->priority >= prio) return;

    if (owner->saved_priority < 0)
        owner->saved_priority = owner->priority;

    owner->priority = prio;
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

static long futex_wait(uint32_t *uaddr, uint32_t val) {
    /* Atomic check: if *uaddr != val, the wake already happened */
    if (__sync_val_compare_and_swap(uaddr, val, val) != val)
        return -EAGAIN;

    /* Spin-yield: not true blocking, but functionally correct.
     * 1000 iterations * PAUSE (~100 cycles @ 2GHz) ≈ 50us.
     * If still not woken, return -ETIMEDOUT so libc can retry. */
    for (int i = 0; i < 1000; i++) {
        if (__sync_val_compare_and_swap(uaddr, val, val) != val)
            return 0; /* value changed — woken */
        __asm__ volatile("pause");
    }

    return -ETIMEDOUT;
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
    thread_t *owner = find_thread_by_tid((int)owner_tid);
    pi_boost(owner, self->priority);

    /* Set FUTEX_WAITERS bit so unlock knows to check the queue */
    __sync_fetch_and_or(uaddr, FUTEX_WAITERS);

    for (int i = 0; i < 10000; i++) {
        old = __sync_val_compare_and_swap(uaddr, 0, tid);
        if (old == 0)
            return 0;
        /* Also try acquiring if only FUTEX_WAITERS bit is set (owner cleared TID) */
        old = __sync_val_compare_and_swap(uaddr, FUTEX_WAITERS, tid | FUTEX_WAITERS);
        if (old == FUTEX_WAITERS)
            return 0;
        __asm__ volatile("pause");
    }

    return -ETIMEDOUT;
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

    int cmd = op & ~FUTEX_PRIVATE_FLAG; /* strip PRIVATE flag — single address space */

    switch (cmd) {
    case FUTEX_WAIT:      return futex_wait(uaddr, val);
    case FUTEX_WAKE:      return futex_wake(uaddr, val);
    case FUTEX_LOCK_PI:   return futex_lock_pi(uaddr);
    case FUTEX_UNLOCK_PI: return futex_unlock_pi(uaddr);
    default:
        serial_puts("futex: unknown op ");
        serial_putchar('0' + (cmd % 10));
        serial_putchar('\n');
        return -ENOSYS;
    }
}
