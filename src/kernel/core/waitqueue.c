/* CosmoRT Waitqueue — kernel-core blocking primitive
 *
 * All state-transitions on the waitee happen under wq->lock. The waker
 * takes the same lock to inspect entries, so there is no window in which
 * a pending wakeup can slip past a sleeping state. This is the invariant
 * the 3x reverted thread_block_ms patches failed to provide — they tried
 * to fix the race with extra CAS/flag tricks instead of a proper lock.
 */

#include "core/waitqueue.h"
#include "core/hrtimer.h"
#include "proc/thread.h"
#include "proc/process.h"
#include "hal/hal.h"

/* ── List helpers (circular doubly-linked, head = first element) ── */

static inline int wq_is_queued(wait_queue_entry_t *e) {
    return e->next != 0;
}

/* Caller holds wq->lock. */
static void wq_insert_tail(wait_queue_head_t *wq, wait_queue_entry_t *e) {
    if (!wq->head) {
        wq->head = e;
        e->next = e;
        e->prev = e;
        return;
    }
    wait_queue_entry_t *tail = wq->head->prev;
    e->prev = tail;
    e->next = wq->head;
    tail->next = e;
    wq->head->prev = e;
}

/* Caller holds wq->lock. */
static void wq_insert_head(wait_queue_head_t *wq, wait_queue_entry_t *e) {
    wq_insert_tail(wq, e);
    wq->head = e; /* rotate so e is the first element */
}

/* Caller holds wq->lock. */
static void wq_remove(wait_queue_head_t *wq, wait_queue_entry_t *e) {
    if (!wq_is_queued(e)) return;
    if (e->next == e) {
        wq->head = 0;
    } else {
        e->prev->next = e->next;
        e->next->prev = e->prev;
        if (wq->head == e) wq->head = e->next;
    }
    e->next = 0;
    e->prev = 0;
}

/* ── Public API ─────────────────────────────── */

void init_waitqueue_head(wait_queue_head_t *wq) {
    wq->lock = (spinlock_t)SPINLOCK_INIT;
    wq->head = 0;
}

void add_wait_queue(wait_queue_head_t *wq, wait_queue_entry_t *e) {
    uint64_t flags;
    spin_lock_irq(&wq->lock, &flags);
    if (!wq_is_queued(e)) {
        e->flags &= ~WQ_FLAG_EXCLUSIVE;
        wq_insert_head(wq, e);
    }
    spin_unlock_irq(&wq->lock, flags);
}

void add_wait_queue_exclusive(wait_queue_head_t *wq, wait_queue_entry_t *e) {
    uint64_t flags;
    spin_lock_irq(&wq->lock, &flags);
    if (!wq_is_queued(e)) {
        e->flags |= WQ_FLAG_EXCLUSIVE;
        wq_insert_tail(wq, e);
    }
    spin_unlock_irq(&wq->lock, flags);
}

void remove_wait_queue(wait_queue_head_t *wq, wait_queue_entry_t *e) {
    uint64_t flags;
    spin_lock_irq(&wq->lock, &flags);
    wq_remove(wq, e);
    spin_unlock_irq(&wq->lock, flags);
}

void prepare_to_wait(wait_queue_head_t *wq, wait_queue_entry_t *e, int state) {
    /* Linux: BOTH list-membership AND state-set inside wq->lock. Wake-Path
     * iterates the list under the same lock, so a sleeper either:
     *   (a) has its state set, gets woken by try_to_wake_up's CAS, OR
     *   (b) hasn't set state yet, but the waker is blocked on wq->lock
     *       and will see THREAD_BLOCKED once we release the lock — the
     *       waker iterates again on next wake_up call (cond re-check
     *       in the sleeper's loop catches the missed event). The classic
     *       wait_event re-check pattern requires the cond to be set
     *       BEFORE wake_up, so a missed wake means cond is already true
     *       and the sleeper bails before schedule(). */
    if (!e->func) e->func = default_wake_function;
    uint64_t flags;
    spin_lock_irq(&wq->lock, &flags);
    if (!wq_is_queued(e)) {
        e->flags &= ~WQ_FLAG_EXCLUSIVE;
        wq_insert_head(wq, e);
    }
    if (e->task) __atomic_store_n(&e->task->state, state, __ATOMIC_RELEASE);
    spin_unlock_irq(&wq->lock, flags);
}

void prepare_to_wait_exclusive(wait_queue_head_t *wq, wait_queue_entry_t *e, int state) {
    if (!e->func) e->func = default_wake_function;
    uint64_t flags;
    spin_lock_irq(&wq->lock, &flags);
    if (!wq_is_queued(e)) {
        e->flags |= WQ_FLAG_EXCLUSIVE;
        wq_insert_tail(wq, e);
    }
    if (e->task) __atomic_store_n(&e->task->state, state, __ATOMIC_RELEASE);
    spin_unlock_irq(&wq->lock, flags);
}

void finish_wait(wait_queue_head_t *wq, wait_queue_entry_t *e) {
    thread_t *t = e->task;
    if (t) __atomic_store_n(&t->state, THREAD_RUNNING, __ATOMIC_RELEASE);

    uint64_t flags;
    spin_lock_irq(&wq->lock, &flags);
    if (wq_is_queued(e)) wq_remove(wq, e);
    spin_unlock_irq(&wq->lock, flags);
}

/* ── Wake ─────────────────────────────────── */

/* Default wait-queue callback: state-CAS via try_to_wake_up. Linux:
 * default_wake_function in kernel/sched/wait.c. Mode is passed straight
 * through; flags/key unused (epoll-style callbacks ignore them).
 * Returns 1 on successful wake (state matched mask), else 0. */
int default_wake_function(wait_queue_entry_t *e, unsigned int mode,
                          int flags, void *key) {
    (void)flags; (void)key;
    return try_to_wake_up(e->task, mode);
}

/* __wake_up: iterate wq list, call e->func(e, mode); count successes;
 * stop after `nr` exclusive wakes (nr<0 = unlimited).
 * Linux: kernel/sched/wait.c::__wake_up_common. */
static int __wake_up_common(wait_queue_head_t *wq, unsigned int mode,
                            int nr, void *key) {
    if (!wq) return 0;
    int woken = 0;
    int exclusive_remaining = nr;   /* -1 = unbounded, >0 = budget */

    uint64_t flags;
    spin_lock_irq(&wq->lock, &flags);

    wait_queue_entry_t *start = wq->head;
    if (start) {
        wait_queue_entry_t *cur = start;
        for (;;) {
            wait_queue_entry_t *next = cur->next;
            int exclusive = (cur->flags & WQ_FLAG_EXCLUSIVE) != 0;
            wait_func_t func = cur->func ? cur->func : default_wake_function;
            int waked = func(cur, mode, 0, key);
            if (waked) {
                woken++;
                if (exclusive) {
                    if (exclusive_remaining > 0) {
                        if (--exclusive_remaining == 0) break;
                    }
                    /* nr==-1: keep waking exclusives too (wake_up_all). */
                }
            }
            if (next == start) break;
            cur = next;
        }
    }

    spin_unlock_irq(&wq->lock, flags);
    return woken;
}

/* Linux wake_up: wake all non-exclusive + first exclusive. */
int wake_up(wait_queue_head_t *wq) {
    return __wake_up_common(wq, TASK_NORMAL, 1, 0);
}

int wake_up_all(wait_queue_head_t *wq) {
    return __wake_up_common(wq, TASK_NORMAL, -1, 0);
}

int wake_up_nr(wait_queue_head_t *wq, int n) {
    if (n <= 0) return 0;
    return __wake_up_common(wq, TASK_NORMAL, n, 0);
}

int wake_up_one(wait_queue_head_t *wq) {
    return __wake_up_common(wq, TASK_NORMAL, 1, 0);
}

int wake_up_interruptible(wait_queue_head_t *wq) {
    return __wake_up_common(wq, TASK_INTERRUPTIBLE_BIT, 1, 0);
}

/* ── Signal-deliverable helper (used by __wait_event_interruptible) ── */

int signal_deliverable(void) {
    thread_t *t = thread_current();
    if (!t || !t->proc) return 0;
    uint64_t pend = t->proc->sig_pending | t->sig_thread_pending;
    return (pend & ~t->sig_blocked) != 0;
}

/* ── schedule_timeout family ─────────────────── */

/* Timer fires in IRQ ctx; uses thread_t as payload so thread_free's
 * hrtimer_cancel_by_data(t) reliably reaps it on zombie cleanup.
 * Wake the target directly via state-CAS — no waitqueue indirection.
 * The sleeper's loop sees state=RUNNABLE, schedule() returns, the
 * post-wake re-check classifies as timeout and breaks out. */
static void wq_timeout_fn(hrtimer_t *timer) {
    thread_t *t = (thread_t *)timer->data;
    if (!t) return;
    (void)try_to_wake_up(t, TASK_NORMAL);
}

/* Internal: park current thread on wq until woken or (optionally) deadline.
 * If deadline_ns == 0, no timeout is armed. `interruptible` causes returns
 * on pending signals before schedule(). */
static long schedule_timeout_common(wait_queue_head_t *wq,
                                    wait_queue_entry_t *e,
                                    uint64_t timeout_ns,
                                    int interruptible) {
    hrtimer_t timer;
    int has_timer = 0;
    uint64_t deadline_ns = 0;

    if (timeout_ns > 0) {
        deadline_ns = hrtimer_now_ns() + timeout_ns;
        hrtimer_init(&timer, wq_timeout_fn, thread_current());
        has_timer = 1;
    }

    long rc = 0;
    for (;;) {
        prepare_to_wait(wq, e, THREAD_BLOCKED);

        if (interruptible && signal_deliverable()) {
            rc = -4 /* EINTR */;
            break;
        }
        if (has_timer && hrtimer_now_ns() >= deadline_ns) {
            rc = -110 /* ETIMEDOUT (Linux) */;
            break;
        }

        if (has_timer) hrtimer_start(&timer, deadline_ns);
        schedule();

        /* Post-wake classification:
         *   - Timeout? return -ETIMEDOUT.
         *   - Signal?  return -EINTR (interruptible only).
         *   - Spurious wake (event_post from unrelated subsystem, SMP IPI,
         *     or explicit wake_up on this wq)? Re-enter the loop and sleep
         *     again up to the original deadline. This matches Linux
         *     wait_event_* semantics where callers handle spurious wakes
         *     via the condition re-check; here the caller is timed-only,
         *     so we simply continue. */
        if (has_timer && hrtimer_now_ns() >= deadline_ns) {
            rc = -110;
            break;
        }
        if (interruptible && signal_deliverable()) {
            rc = -4;
            break;
        }
        /* Spurious wake: keep sleeping. */
    }

    finish_wait(wq, e);
    if (has_timer) hrtimer_cancel(&timer);
    return rc;
}

long schedule_timeout(wait_queue_head_t *wq, wait_queue_entry_t *e, uint64_t timeout_ns) {
    return schedule_timeout_common(wq, e, timeout_ns, 0);
}

long schedule_timeout_interruptible(wait_queue_head_t *wq, wait_queue_entry_t *e, uint64_t timeout_ns) {
    return schedule_timeout_common(wq, e, timeout_ns, 1);
}

/* Generic timed sleep: waitqueue-backed, signal-interruptible, ns precision.
 * Uses a caller-local head (stack), so per-thread, no global state.
 * Returns -EINTR on signal, 0 on full sleep.
 *
 * This is the waitqueue-proper replacement for thread_block_ms's naked
 * state=BLOCKED + schedule() pattern that leaked the missed-wakeup race. */
int sleep_interruptible_ns(uint64_t timeout_ns) {
    if (timeout_ns == 0) return 0;

    /* Fast path: already pending signal -> no block at all. */
    if (signal_deliverable()) return -4 /* EINTR */;

    wait_queue_head_t wq;
    init_waitqueue_head(&wq);

    DEFINE_WAIT(ent);

    long rc = schedule_timeout_interruptible(&wq, &ent, timeout_ns);
    if (rc == -4) return -4;
    /* ETIMEDOUT in schedule_timeout is the normal end-of-sleep for this
     * primitive — caller asked for a sleep, they got it. */
    return 0;
}
