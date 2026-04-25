/* CosmoRT Event Queue — kernel-side event_post + event_wait
 *
 * event_post: write event into target thread's queue, then sched_wake.
 * event_wait: consume next event; if empty, block via schedule().
 * Timeouts use hrtimer (LAPIC one-shot) instead of polling.
 *
 * Ring grows on overflow via page_alloc — unbounded like Linux wait_queue.
 */

#include "core/event_queue.h"
#include "core/hrtimer.h"
#include "core/waitqueue.h"
#include "proc/thread.h"
#include "proc/process.h"
#include "core/percpu.h"
#include "core/timer.h"
#include "mm/page_alloc.h"
#include "arch/arch.h"
#include "hal/hal.h"
#include "spinlock.h"

/* Size of one page in bytes — grow step is always page-aligned. */
#define EQ_PAGE_BYTES 4096

_Static_assert((EQ_INIT_CAPACITY & (EQ_INIT_CAPACITY - 1)) == 0,
               "EQ_INIT_CAPACITY must be power of 2");
_Static_assert(sizeof(event_t) * EQ_INIT_CAPACITY <= EQ_PAGE_BYTES,
               "EQ_INIT_CAPACITY must fit in one page");

static inline int eq_pages_for(uint32_t capacity) {
    uint64_t bytes = (uint64_t)capacity * sizeof(event_t);
    int pages = (int)((bytes + EQ_PAGE_BYTES - 1) / EQ_PAGE_BYTES);
    return pages < 1 ? 1 : pages;
}

void event_queue_init(event_queue_t *eq) {
    eq->capacity = EQ_INIT_CAPACITY;
    eq->mask = EQ_INIT_CAPACITY - 1;
    eq->head = 0;
    eq->tail = 0;
    eq->events = (event_t *)pages_alloc(eq_pages_for(EQ_INIT_CAPACITY));
    init_waitqueue_head(&eq->wq);
}

void event_queue_destroy(event_queue_t *eq) {
    if (!eq->events) return;
    pages_free(eq->events, eq_pages_for(eq->capacity));
    eq->events = (event_t *)0;
    eq->capacity = 0;
    eq->mask = 0;
    eq->head = 0;
    eq->tail = 0;
    /* wq is embedded; no separate cleanup. The owner thread (single
     * consumer) cannot be parked here at destroy time, so no waiter
     * teardown is needed. */
}

/* Grow the ring to new_capacity (power of 2, > old capacity).
 * Caller must hold eq_lock. Copies pending events compactly.
 * On allocation failure: returns -1, queue unchanged (caller falls back
 * to lossy overwrite — matches Linux oom_kill semantics better than panic). */
static int eq_grow_locked(event_queue_t *eq, uint32_t new_capacity) {
    event_t *new_events = (event_t *)pages_alloc(eq_pages_for(new_capacity));
    if (!new_events) return -1;

    uint32_t h = eq->head;
    uint32_t t = eq->tail;
    uint32_t pending = h - t;
    uint32_t new_mask = new_capacity - 1;

    for (uint32_t i = 0; i < pending; i++)
        new_events[i] = eq->events[(t + i) & eq->mask];

    event_t *old_events = eq->events;
    int old_pages = eq_pages_for(eq->capacity);

    eq->events = new_events;
    eq->capacity = new_capacity;
    eq->mask = new_mask;
    eq->tail = 0;
    hal_cpu_store_release(&eq->head, pending);

    pages_free(old_events, old_pages);
    return 0;
}

/* ── Post: write event + wake target ─────────── */

void event_post(thread_t *target, uint32_t type, uint64_t data) {
    if (!target) return;
    event_queue_t *eq = &target->eq;

    uint64_t irqf;
    spin_lock_irq(&target->eq_lock, &irqf);

    if (!eq->events) {
        spin_unlock_irq(&target->eq_lock, irqf);
        return;
    }

    uint32_t h = eq->head;
    uint32_t t = eq->tail;

    if (__builtin_expect(h - t >= eq->capacity, 0)) {
        uint32_t next_cap = eq->capacity * 2;
        if (next_cap > eq->capacity && eq_grow_locked(eq, next_cap) == 0) {
            h = eq->head;
            t = eq->tail;
        } else {
            hal_cpu_store_release(&eq->tail, t + 1);
        }
    }

    eq->events[h & eq->mask].type = type;
    eq->events[h & eq->mask].data = data;
    hal_cpu_store_release(&eq->head, h + 1);

    spin_unlock_irq(&target->eq_lock, irqf);

    /* Wake the eq's owner if parked in event_wait_ns. wake_up_interruptible
     * acquires eq->wq.lock — same lock event_wait_ns holds during
     * prepare_to_wait. That serialization closes the missed-wakeup race
     * the old "sched_wake(target)" path had between event_wait_ns's
     * cur->state=BLOCKED and the schedule() call.
     *
     * Wir behalten den sched_wake(target)-Pfad solange event_wait_ns
     * noch nicht via prepare_to_wait an eq->wq gebunden ist — wird im
     * naechsten Commit entfernt sobald event_wait_ns ein wq-Waiter ist. */
    wake_up_interruptible(&eq->wq);

    extern void sched_wake(struct thread *t);
    sched_wake(target);
}

/* ── hrtimer timeout callback: wake blocked thread ── */

static void timeout_wake(hrtimer_t *timer) {
    thread_t *t = (thread_t *)timer->data;
    extern void sched_wake(thread_t *t);
    sched_wake(t);
}

/* ── Block: pure timeout sleep, waitqueue-backed ──
 * Replaces the old naked state=BLOCKED+schedule() pattern that had a
 * missed-wakeup race between the pending-check and state=BLOCKED. The
 * waitqueue primitive serializes both under its lock. sleep_interruptible_ns
 * handles signals, timeout and race-free wakeups. */

void thread_block_ms(int timeout_ms) {
    if (timeout_ms <= 0) return;
    (void)timeout_wake;
    (void)sleep_interruptible_ns((uint64_t)timeout_ms * NSEC_PER_MSEC);
}

/* ── Wait: consume next event, block if empty ── */

int event_wait_ns(event_queue_t *eq, event_t *out, int64_t timeout_ns) {
    thread_t *cur = thread_current();
    if (!cur) return -14; /* EFAULT */

    hrtimer_t timer;
    int has_timer = 0;
    uint64_t deadline_ns = 0;
    if (timeout_ns > 0) {
        deadline_ns = hrtimer_now_ns() + (uint64_t)timeout_ns;
        hrtimer_init(&timer, timeout_wake, cur);
        has_timer = 1;
    }

    for (;;) {
        /* Signal check — fatal signals (SIGALRM, SIGKILL) must interrupt.
         * Ignore SIG_DFL-ignored signals (SIGCHLD=17, SIGURG=23,
         * SIGWINCH=28, SIGIO=29) to avoid spurious EINTR. */
        if (cur->proc) {
            uint64_t all_pending = cur->proc->sig_pending | cur->sig_thread_pending;
            uint64_t deliverable = all_pending & ~cur->sig_blocked;
            if (deliverable) {
                #define SIG_DFL_IGNORE ((1ULL << 16) | (1ULL << 22) | (1ULL << 27) | (1ULL << 28))
                uint64_t real = deliverable;
                for (int s = 1; s < 64 && real; s++) {
                    if (!(real & (1ULL << (s-1)))) continue;
                    if ((uint64_t)cur->proc->sig_actions[s].sa_handler == 0 &&
                        ((1ULL << (s-1)) & SIG_DFL_IGNORE))
                        real &= ~(1ULL << (s-1));
                }
                if (real) {
                    if (has_timer) hrtimer_cancel(&timer);
                    return -4; /* EINTR */
                }
            }
        }

        uint32_t h = hal_cpu_load_acquire(&eq->head);
        uint32_t t = eq->tail;

        if (h != t) {
            *out = eq->events[t & eq->mask];
            hal_cpu_store_release(&eq->tail, t + 1);
            if (has_timer) hrtimer_cancel(&timer);
            return 0;
        }

        if (timeout_ns == 0)
            return -11; /* EAGAIN */

        if (has_timer && hrtimer_now_ns() >= deadline_ns) {
            hrtimer_cancel(&timer);
            return -11; /* EAGAIN (timeout) */
        }

        if (has_timer)
            hrtimer_start(&timer, deadline_ns);

        cur->state = THREAD_BLOCKED;

        /* Close race: event arrived between fast-path and BLOCKED */
        hal_cpu_mfence();
        if (hal_cpu_load_acquire(&eq->head) != eq->tail) {
            cur->state = THREAD_RUNNING;
            continue;
        }

        extern void schedule(void);
        schedule();

        if (has_timer && hrtimer_now_ns() >= deadline_ns) {
            hrtimer_cancel(&timer);
            h = hal_cpu_load_acquire(&eq->head);
            if (h != eq->tail) {
                *out = eq->events[eq->tail & eq->mask];
                hal_cpu_store_release(&eq->tail, eq->tail + 1);
                return 0;
            }
            return -11; /* EAGAIN (timeout) */
        }
    }
}

int event_wait(event_queue_t *eq, event_t *out, int timeout_ms) {
    if (timeout_ms < 0) return event_wait_ns(eq, out, -1);
    return event_wait_ns(eq, out, (int64_t)timeout_ms * (int64_t)NSEC_PER_MSEC);
}
