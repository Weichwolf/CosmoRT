/* CosmoRT Futex — Fast Userspace Mutex with Priority Inheritance
 *
 * True blocking via event_wait/event_post + syscall restart.
 * The fast path (uncontended) never enters the kernel. Only contended
 * locks hit this code, and they're rare.
 *
 * Timeout: converted from struct timespec to milliseconds for event_wait.
 * On timeout, the thread is removed from the wait queue and -ETIMEDOUT returned.
 *
 * PI (Priority Inheritance): when a high-priority thread blocks on a PI futex
 * owned by a low-priority thread, the owner is temporarily boosted. This prevents
 * priority inversion per IEEE 1003.1b PTHREAD_PRIO_INHERIT.
 */

#include "ipc/futex.h"
#include "sys/syscall.h"
#include "proc/thread.h"
#include "proc/process.h"
#include "core/percpu.h"
#include "spinlock.h"
#include "mm/slab.h"
#include "hw/serial.h"
#include "config.h"
#include "arch/arch.h"
#include "core/event_queue.h"
#include "uaccess.h"

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

/* Remove a specific thread from a futex wait queue bucket.
 * Returns 1 if found and removed, 0 if not found. */
static int futex_remove_waiter(uint64_t addr, uint32_t pid, thread_t *t) {
    int bucket = hash_uaddr(addr, pid);
    uint64_t flags;
    spin_lock_irq(&futex_hash[bucket].lock, &flags);

    futex_waiter_t **pp = &futex_hash[bucket].head;
    while (*pp) {
        futex_waiter_t *w = *pp;
        if (w->uaddr == addr && w->pid == pid && w->thread == t) {
            *pp = w->next;
            slab_free(&waiter_slab, w);
            spin_unlock_irq(&futex_hash[bucket].lock, flags);
            return 1;
        }
        pp = &w->next;
    }

    spin_unlock_irq(&futex_hash[bucket].lock, flags);
    return 0;
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
}

/* Restore owner's original priority after PI unlock. */
static void pi_unboost(thread_t *owner) {
    if (!owner) return;
    if (owner->saved_priority < 0) return;

    owner->priority = owner->saved_priority;
    owner->saved_priority = -1;
}

/* Drain stale futex-related events from a thread's event queue.
 * Called on syscall restart to consume the event that caused the wake.
 * Returns: EQ_FUTEX_WAKE if woken by futex_wake,
 *          EQ_TIMEOUT if woken by timeout,
 *          0 if no relevant event found. */
static uint32_t futex_drain_events(event_queue_t *eq) {
    event_t ev;
    uint32_t result = 0;

    /* Drain all futex/timeout events — there should be at most one,
     * but be defensive against duplicates. Prefer TIMEOUT over WAKE
     * only if no WAKE seen (WAKE means someone explicitly woke us). */
    while (eq_pop(eq, &ev) == 0) {
        if (ev.type == EQ_FUTEX_WAKE) {
            result = EQ_FUTEX_WAKE;
        } else if (ev.type == EQ_TIMEOUT && result != EQ_FUTEX_WAKE) {
            result = EQ_TIMEOUT;
        }
        /* Other event types: lost. This is acceptable because futex_wait
         * is a blocking syscall — only futex events are relevant here.
         * In practice, other events don't arrive during futex wait. */
    }
    return result;
}

/* ── FUTEX_WAIT ─────────────────────────────────── */

extern uint64_t pml4[];

static long futex_wait(uint32_t *uaddr, uint32_t val, int timeout_ms) {
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    process_t *p = t->proc;
    uint32_t pid = p ? p->pid : 0;
    uint64_t addr = (uint64_t)(uintptr_t)uaddr;

    /* On syscall restart: an event woke us. Drain it to determine why. */
    uint32_t wake_reason = futex_drain_events(&t->eq);

    if (wake_reason == EQ_TIMEOUT) {
        /* Timeout fired — remove ourselves from the wait queue (if still there)
         * and return -ETIMEDOUT. futex_wake may have raced and already removed us,
         * in which case futex_remove_waiter returns 0 — that's fine. */
        futex_remove_waiter(addr, pid, t);
        return -ETIMEDOUT;
    }

    if (wake_reason == EQ_FUTEX_WAKE) {
        /* Woken by futex_wake — it already removed us from the wait queue.
         * The *uaddr check below will return -EAGAIN (value changed). */
    }

    /* Atomic check: if *uaddr != val, the wake already happened */
    if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val)
        return -EAGAIN;

    /* Add to wait queue under lock */
    int bucket = hash_uaddr(addr, pid);
    uint64_t flags;
    spin_lock_irq(&futex_hash[bucket].lock, &flags);

    /* Re-check under lock — value may have changed */
    if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val) {
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
    w->uaddr = addr;
    w->pid = pid;
    w->next = futex_hash[bucket].head;
    futex_hash[bucket].head = w;

    spin_unlock_irq(&futex_hash[bucket].lock, flags);

    /* Block via event_wait — futex_wake will event_post us.
     * On wake, syscall restarts: futex_wait re-enters, drains the event,
     * and returns -EAGAIN (value changed) or -ETIMEDOUT (timeout). */
    {
        event_t ev;
        int _wr = event_wait(&t->eq, &ev, timeout_ms);
        if (_wr == -4) return -EINTR;
    }
    return 0; /* unreachable — event_wait does syscall restart when blocking */
}

/* ── FUTEX_WAKE ─────────────────────────────────── */

static long futex_wake(uint32_t *uaddr, uint32_t max_wake) {
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
            /* Remove from queue + wake */
            thread_t *target = w->thread;
            *pp = w->next;
            slab_free(&waiter_slab, w);
            event_post(target, EQ_FUTEX_WAKE, 0);
            woken++;
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

    /* Deadlock: we already own it (PTHREAD_MUTEX_ERRORCHECK) */
    if ((old & FUTEX_TID_MASK) == tid)
        return -EDEADLK;

    /* Set FUTEX_WAITERS bit so unlock knows to check the queue */
    __sync_fetch_and_or(uaddr, FUTEX_WAITERS);

    /* Drain stale events from previous restart */
    futex_drain_events(&self->eq);

    /* Block via wait queue */
    {
        process_t *p = self->proc;
        uint32_t pid = p ? p->pid : 0;
        int bucket = hash_uaddr((uint64_t)(uintptr_t)uaddr, pid);
        uint64_t flags;
        spin_lock_irq(&futex_hash[bucket].lock, &flags);

        /* Boost owner under lock so it can't be freed between find and boost */
        uint32_t owner_tid = old & FUTEX_TID_MASK;
        thread_t *owner = thread_find_by_tid((int)owner_tid);
        if (owner)
            pi_boost(owner, self->priority);

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

        event_t ev;
        int _wr = event_wait(&self->eq, &ev, -1);
        if (_wr == -4) return -EINTR;
    }

    return 0; /* unreachable — syscall restarts */
}

/* ── FUTEX_UNLOCK_PI ────────────────────────────── */

static long futex_unlock_pi(uint32_t *uaddr) {
    thread_t *self = thread_current();
    if (!self) return -EFAULT;
    uint32_t tid = (uint32_t)self->tid;

    /* Verify we own this futex */
    uint32_t cur = __atomic_load_n(uaddr, __ATOMIC_ACQUIRE);
    if ((cur & FUTEX_TID_MASK) != tid)
        return -EPERM;

    /* Restore our priority */
    pi_unboost(self);

    /* Release: clear our TID */
    if (cur & FUTEX_WAITERS) {
        /* Waiters present — clear TID, keep WAITERS bit, wake one */
        __sync_val_compare_and_swap(uaddr, cur, FUTEX_WAITERS);
        futex_wake(uaddr, 1);
    } else {
        /* No waiters — just release */
        __sync_val_compare_and_swap(uaddr, cur, 0);
    }

    return 0;
}

/* ── Timespec → milliseconds ────────────────────── */

/* Convert userspace struct timespec to milliseconds.
 * Returns timeout_ms for event_wait: -1 if ts is NULL (infinite),
 * 0 if timespec is zero, >0 otherwise. Capped at INT32_MAX. */
static int timespec_to_ms(const void *ts) {
    if (!ts) return -1;

    struct { long tv_sec; long tv_nsec; } kts;
    if (copy_from_user(&kts, ts, sizeof(kts)) != 0)
        return -1; /* bad pointer → treat as infinite (caller will fail on uaddr access) */

    if (kts.tv_sec < 0 || kts.tv_nsec < 0) return 0;

    long ms = kts.tv_sec * 1000 + kts.tv_nsec / 1000000;
    if (ms <= 0 && (kts.tv_sec > 0 || kts.tv_nsec > 0))
        ms = 1; /* sub-millisecond → round up to 1ms */
    if (ms > 0x7FFFFFFFL) ms = 0x7FFFFFFFL; /* cap */
    return (int)ms;
}

/* ── FUTEX_REQUEUE / FUTEX_CMP_REQUEUE ─────────── */

/* Requeue: wake up to wake_max waiters on uaddr1, then move up to
 * requeue_max remaining waiters from uaddr1's queue to uaddr2's queue
 * WITHOUT waking them. They'll wake when someone does FUTEX_WAKE on uaddr2.
 *
 * CMP variant: atomically checks *uaddr1 == val3 first (prevents ABA race
 * between userspace check and kernel requeue). Returns -EAGAIN on mismatch.
 *
 * Linux: timeout parameter is reused as val2 (requeue_max), not a pointer. */

static long futex_requeue(uint32_t *uaddr1, uint32_t wake_max,
                          uint32_t requeue_max, uint32_t *uaddr2,
                          int cmp, uint32_t cmpval) {
    process_t *p = proc_current();
    uint32_t pid = p ? p->pid : 0;
    uint64_t addr1 = (uint64_t)(uintptr_t)uaddr1;
    uint64_t addr2 = (uint64_t)(uintptr_t)uaddr2;

    int b1 = hash_uaddr(addr1, pid);
    int b2 = hash_uaddr(addr2, pid);

    /* Lock both buckets — ordered by index to prevent deadlock */
    uint64_t flags;
    if (b1 == b2) {
        spin_lock_irq(&futex_hash[b1].lock, &flags);
    } else if (b1 < b2) {
        spin_lock_irq(&futex_hash[b1].lock, &flags);
        spin_lock(&futex_hash[b2].lock);
    } else {
        spin_lock_irq(&futex_hash[b2].lock, &flags);
        spin_lock(&futex_hash[b1].lock);
    }

    /* CMP_REQUEUE: check *uaddr1 == cmpval under lock */
    if (cmp) {
        if (__atomic_load_n(uaddr1, __ATOMIC_ACQUIRE) != cmpval) {
            if (b1 == b2) {
                spin_unlock_irq(&futex_hash[b1].lock, flags);
            } else if (b1 < b2) {
                spin_unlock(&futex_hash[b2].lock);
                spin_unlock_irq(&futex_hash[b1].lock, flags);
            } else {
                spin_unlock(&futex_hash[b1].lock);
                spin_unlock_irq(&futex_hash[b2].lock, flags);
            }
            return -EAGAIN;
        }
    }

    long woken = 0, requeued = 0;

    /* Phase 1: wake up to wake_max waiters on uaddr1 */
    futex_waiter_t **pp = &futex_hash[b1].head;
    while (*pp && (uint32_t)woken < wake_max) {
        futex_waiter_t *w = *pp;
        if (w->uaddr == addr1 && w->pid == pid) {
            thread_t *target = w->thread;
            *pp = w->next;
            slab_free(&waiter_slab, w);
            event_post(target, EQ_FUTEX_WAKE, 0);
            woken++;
        } else {
            pp = &(*pp)->next;
        }
    }

    /* Phase 2: move up to requeue_max remaining waiters to uaddr2 */
    pp = &futex_hash[b1].head;
    while (*pp && (uint32_t)requeued < requeue_max) {
        futex_waiter_t *w = *pp;
        if (w->uaddr == addr1 && w->pid == pid) {
            /* Remove from b1 */
            *pp = w->next;
            /* Update address and insert into b2 */
            w->uaddr = addr2;
            w->next = futex_hash[b2].head;
            futex_hash[b2].head = w;
            requeued++;
        } else {
            pp = &(*pp)->next;
        }
    }

    /* Unlock in reverse order */
    if (b1 == b2) {
        spin_unlock_irq(&futex_hash[b1].lock, flags);
    } else if (b1 < b2) {
        spin_unlock(&futex_hash[b2].lock);
        spin_unlock_irq(&futex_hash[b1].lock, flags);
    } else {
        spin_unlock(&futex_hash[b1].lock);
        spin_unlock_irq(&futex_hash[b2].lock, flags);
    }

    return woken + requeued;
}

/* ── Dispatcher ─────────────────────────────────── */

long do_futex(uint32_t *uaddr, int op, uint32_t val,
              const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3) {
    int cmd = op & ~(FUTEX_PRIVATE_FLAG | 0x100); /* strip PRIVATE + CLOCK_REALTIME */

    switch (cmd) {
    case FUTEX_WAIT:
        return futex_wait(uaddr, val, timespec_to_ms(timeout));
    case FUTEX_WAKE:
        return futex_wake(uaddr, val);
    case FUTEX_REQUEUE:
        return futex_requeue(uaddr, val, (uint32_t)(uintptr_t)timeout,
                             uaddr2, 0, 0);
    case FUTEX_CMP_REQUEUE:
        return futex_requeue(uaddr, val, (uint32_t)(uintptr_t)timeout,
                             uaddr2, 1, val3);
    case FUTEX_LOCK_PI:
        return futex_lock_pi(uaddr);
    case FUTEX_UNLOCK_PI:
        return futex_unlock_pi(uaddr);
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
    case 9: /* FUTEX_WAIT_BITSET — treat as FUTEX_WAIT (ignore bitmask) */
        return futex_wait(uaddr, val, timespec_to_ms(timeout));
    case 10: /* FUTEX_WAKE_BITSET — treat as FUTEX_WAKE */
        return futex_wake(uaddr, val);
    default:
        serial_puts("futex: unknown op ");
        serial_hex64((uint64_t)(unsigned)cmd);
        serial_putchar('\n');
        return -ENOSYS;
    }
}
