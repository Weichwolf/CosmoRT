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

    /* Wake the consumer via the queue's own waitqueue. The sleeper
     * parked on eq->wq via prepare_to_wait — wake_up_interruptible
     * flips its state RUNNABLE under the wq lock, ending the
     * missed-wakeup race that earlier sched_wake-routing variants
     * never fully closed. */
    wake_up_interruptible(&eq->wq);
}

/* SIG_DFL-ignored signals (SIGCHLD=17, SIGURG=23, SIGWINCH=28, SIGIO=29):
 * blocked sleepers must not return EINTR for these. A pending bit can be
 * set without a registered handler — Linux's quiet wakeup behaviour. */
#define EQ_SIG_DFL_IGNORE ((1ULL << 16) | (1ULL << 22) | (1ULL << 27) | (1ULL << 28))

static int eq_signal_pending(thread_t *cur) {
    if (!cur || !cur->proc) return 0;
    uint64_t all = cur->proc->sig_pending | cur->sig_thread_pending;
    uint64_t deliverable = all & ~cur->sig_blocked;
    if (!deliverable) return 0;
    uint64_t real = deliverable;
    for (int s = 1; s < 64 && real; s++) {
        if (!(real & (1ULL << (s - 1)))) continue;
        if ((uint64_t)cur->proc->sig_actions[s].sa_handler == 0 &&
            ((1ULL << (s - 1)) & EQ_SIG_DFL_IGNORE))
            real &= ~(1ULL << (s - 1));
    }
    return real != 0;
}

/* ── Block: pure timeout sleep, waitqueue-backed ── */

void thread_block_ms(int timeout_ms) {
    if (timeout_ms <= 0) return;
    (void)sleep_interruptible_ns((uint64_t)timeout_ms * NSEC_PER_MSEC);
}

/* hrtimer wake callback: re-target the queue's wq so the parked sleeper
 * wakes up uniformly via the wait-queue path (no thread->wait_head
 * pointer dance). data points at the eq itself. */
static void eq_timeout_wake(hrtimer_t *timer) {
    event_queue_t *eq = (event_queue_t *)timer->data;
    if (eq) wake_up_interruptible(&eq->wq);
}

/* ── Wait: consume next event, block if empty ──
 *
 * Linux wait_event_interruptible_timeout pattern: prepare_to_wait
 * publishes BLOCKED + queues the entry under wq->lock. event_post takes
 * the same lock, sees the queued entry, and flips state RUNNABLE. There
 * is no sched_wake-via-thread->wait_head routing — every event source
 * (futex_wake, kill_one, pipe_write, …) uses event_post which targets
 * eq->wq directly. */
int event_wait(event_queue_t *eq, event_t *out, int timeout_ms) {
    thread_t *cur = thread_current();
    if (!cur) return -14; /* EFAULT */

    /* Fast path: event already queued. */
    if (hal_cpu_load_acquire(&eq->head) != eq->tail) {
        uint32_t t = eq->tail;
        *out = eq->events[t & eq->mask];
        hal_cpu_store_release(&eq->tail, t + 1);
        return 0;
    }
    if (timeout_ms == 0) return -11; /* EAGAIN */
    if (eq_signal_pending(cur)) return -4; /* EINTR */

    hrtimer_t timer;
    int has_timer = 0;
    uint64_t deadline_ns = 0;
    if (timeout_ms > 0) {
        deadline_ns = hrtimer_now_ns() + ms_to_ns((uint64_t)timeout_ms);
        hrtimer_init(&timer, eq_timeout_wake, eq);
        hrtimer_start(&timer, deadline_ns);
        has_timer = 1;
    }

    DEFINE_WAIT(ent);
    int rc;
    for (;;) {
        prepare_to_wait(&eq->wq, &ent, THREAD_BLOCKED);

        if (hal_cpu_load_acquire(&eq->head) != eq->tail) {
            uint32_t t = eq->tail;
            *out = eq->events[t & eq->mask];
            hal_cpu_store_release(&eq->tail, t + 1);
            rc = 0;
            break;
        }
        if (eq_signal_pending(cur)) { rc = -4 /* EINTR */; break; }
        if (has_timer && hrtimer_now_ns() >= deadline_ns) {
            rc = -11 /* EAGAIN (timeout) */;
            break;
        }

        schedule();
        /* Loop and re-evaluate. prepare_to_wait sets state again; if a
         * spurious wake happened (signal that filtered out, ipi), we
         * just sleep again until the deadline or a real event. */
    }
    finish_wait(&eq->wq, &ent);
    if (has_timer) hrtimer_cancel(&timer);
    return rc;
}
