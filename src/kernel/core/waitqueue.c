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

extern void sched_add(struct thread *t);

/* Linux's einzige Wake-Primitive. Mask-Filter via (1u << t->state) & mask:
 *   TASK_INTERRUPTIBLE -> matcht THREAD_BLOCKED
 *   TASK_STOPPED       -> matcht THREAD_STOPPED
 *   TASK_NORMAL        -> beide
 * Bei Match: CAS state -> RUNNABLE, sched_add. Returns 1 bei Wake, 0 sonst.
 *
 * Loop ist Race-Fix: zwischen Load und CAS kann der Thread seinen state
 * von BLOCKED nach STOPPED (oder umgekehrt) wechseln. Single-CAS mit
 * fixiertem Erwartungswert wuerde fehlschlagen und einen verbleibenden
 * im-Mask-State ungeweckt lassen — das war der 10.2a-WIP-Bug, der
 * event_queue/futex/getdents-Tests in TIMEOUT trieb. Linux's pi_lock-
 * Modell vermeidet den Loop durch Lock-Akquisition; wir nehmen die CAS-
 * Loop-Variante, weil unsere Wake-Pfade (sched_wake, signal_wake_up)
 * keinen pi_lock-Aequivalenten haben. */
int try_to_wake_up(thread_t *t, unsigned int mask) {
    if (!t) return 0;
    for (;;) {
        int st = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
        if (st == THREAD_DEAD || st == THREAD_FREE) return 0;
        if (!((1u << st) & mask)) return 0;
        if (__sync_bool_compare_and_swap(&t->state, st, THREAD_RUNNABLE)) {
            sched_add(t);
            return 1;
        }
    }
}

int default_wake_function(wait_queue_entry_t *e, unsigned int mask) {
    return try_to_wake_up(e->task, mask);
}

int autoremove_wake_function(wait_queue_entry_t *e, unsigned int mask) {
    int r = try_to_wake_up(e->task, mask);
    if (r) e->flags |= WQ_FLAG_AUTOREMOVE;
    return r;
}

void signal_wake_up(thread_t *t) {
    try_to_wake_up(t, TASK_INTERRUPTIBLE | TASK_KILLABLE);
}

/* Dispatch via e->func — Subsysteme registrieren eigene Callbacks (epoll).
 * Default ist autoremove_wake_function ueber DEFINE_WAIT. NULL-Func wird
 * als default_wake_function behandelt (defensive bei manuell konstruierten
 * Eintraegen ohne DEFINE_WAIT). */
static inline int wq_call(wait_queue_entry_t *e, unsigned int mask) {
    wait_func_t fn = e->func;
    if (!fn) fn = default_wake_function;
    return fn(e, mask);
}

int wake_up_nr(wait_queue_head_t *wq, int n) {
    if (n <= 0) return 0;
    int woken = 0;

    uint64_t flags;
    spin_lock_irq(&wq->lock, &flags);

    wait_queue_entry_t *start = wq->head;
    if (start) {
        wait_queue_entry_t *cur = start;
        for (;;) {
            wait_queue_entry_t *next = cur->next;
            int exclusive = (cur->flags & WQ_FLAG_EXCLUSIVE) != 0;

            if (wq_call(cur, TASK_NORMAL)) {
                woken++;
                if (exclusive && woken >= n) break;
            }
            if (next == start) break;
            cur = next;
        }
    }

    spin_unlock_irq(&wq->lock, flags);
    return woken;
}

int wake_up(wait_queue_head_t *wq) {
    /* Linux wake_up: wake all non-exclusive + first exclusive (nr_exclusive=1) */
    if (!wq) return 0;
    int woken = 0;

    uint64_t flags;
    spin_lock_irq(&wq->lock, &flags);

    wait_queue_entry_t *start = wq->head;
    if (start) {
        wait_queue_entry_t *cur = start;
        int exclusive_woken = 0;
        for (;;) {
            wait_queue_entry_t *next = cur->next;
            int exclusive = (cur->flags & WQ_FLAG_EXCLUSIVE) != 0;

            if (!(exclusive && exclusive_woken)) {
                if (wq_call(cur, TASK_NORMAL)) {
                    woken++;
                    if (exclusive) exclusive_woken = 1;
                }
            }
            if (next == start) break;
            cur = next;
        }
    }

    spin_unlock_irq(&wq->lock, flags);
    return woken;
}

int wake_up_all(wait_queue_head_t *wq) {
    if (!wq) return 0;
    int woken = 0;

    uint64_t flags;
    spin_lock_irq(&wq->lock, &flags);

    wait_queue_entry_t *start = wq->head;
    if (start) {
        wait_queue_entry_t *cur = start;
        for (;;) {
            wait_queue_entry_t *next = cur->next;
            if (wq_call(cur, TASK_NORMAL)) woken++;
            if (next == start) break;
            cur = next;
        }
    }

    spin_unlock_irq(&wq->lock, flags);
    return woken;
}

int wake_up_one(wait_queue_head_t *wq) { return wake_up_nr(wq, 1); }

int wake_up_interruptible(wait_queue_head_t *wq) {
    if (!wq) return 0;
    int woken = 0;

    uint64_t flags;
    spin_lock_irq(&wq->lock, &flags);

    wait_queue_entry_t *start = wq->head;
    if (start) {
        wait_queue_entry_t *cur = start;
        int exclusive_woken = 0;
        for (;;) {
            wait_queue_entry_t *next = cur->next;
            int exclusive = (cur->flags & WQ_FLAG_EXCLUSIVE) != 0;

            if (!(exclusive && exclusive_woken)) {
                if (wq_call(cur, TASK_INTERRUPTIBLE)) {
                    woken++;
                    if (exclusive) exclusive_woken = 1;
                }
            }
            if (next == start) break;
            cur = next;
        }
    }

    spin_unlock_irq(&wq->lock, flags);
    return woken;
}

/* ── Signal-deliverable helper (used by __wait_event_interruptible) ── */

/* SIG_DFL signals whose default action is "ignore" — these must NOT
 * interrupt blocking syscalls, even if pending+unblocked. Linux's
 * recalc_sigpending() drops them from the pending set when the action
 * is SIG_DFL; we filter at the deliverable-check instead.
 *
 * Without this filter, a process whose child exits during a nanosleep
 * (SIGCHLD pending, SIG_DFL handler) is woken with -EINTR + an
 * ERESTART_RESTARTBLOCK that cannot be cleared (the bit stays set, the
 * restart immediately re-EINTRs). That is the alarm07 → clock_nanosleep01
 * livelock: alarm07 leaves a SIGCHLD pending in the LTP-runner-parent's
 * sig_pending; the next syscall that calls signal_deliverable() loops. */
#define SIG_DFL_IGNORE_MASK ((1ULL << 16) | (1ULL << 22) | (1ULL << 27) | (1ULL << 28))

int signal_deliverable(void) {
    thread_t *t = thread_current();
    if (!t || !t->proc) return 0;
    uint64_t pend = (t->proc->sig_pending | t->sig_thread_pending) & ~t->sig_blocked;
    if (!pend) return 0;
    /* Filter out SIG_DFL_IGNORE signals whose handler is still SIG_DFL. */
    uint64_t mask_to_check = pend & SIG_DFL_IGNORE_MASK;
    while (mask_to_check) {
        int s = __builtin_ctzll(mask_to_check) + 1;
        mask_to_check &= mask_to_check - 1;
        if ((uint64_t)t->proc->sig_actions[s].sa_handler == 0)
            pend &= ~SIG_BIT(s);
    }
    return pend != 0;
}

/* ── schedule_timeout family ─────────────────── */

/* Timer fires in IRQ ctx; uses thread_t as payload so thread_free's
 * hrtimer_cancel_by_data(t) reliably reaps it on zombie cleanup.
 * Direct state-CAS via sched_wake — no waitqueue routing. The sleeper
 * loop in schedule_timeout_common re-evaluates the deadline after the
 * wake and breaks out with -ETIMEDOUT. */
extern void sched_wake(thread_t *t);
static void wq_timeout_fn(hrtimer_t *timer) {
    thread_t *t = (thread_t *)timer->data;
    if (t) sched_wake(t);
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
