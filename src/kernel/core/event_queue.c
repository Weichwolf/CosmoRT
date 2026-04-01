/* CosmoRT Event Queue — kernel-side event_post + event_wait
 *
 * event_post: write event into target thread's queue, then sched_wake.
 * event_wait: consume next event; if empty, block via schedule().
 * Timeouts use hrtimer (LAPIC one-shot) instead of polling.
 */

#include "core/event_queue.h"
#include "core/hrtimer.h"
#include "proc/thread.h"
#include "proc/process.h"
#include "core/percpu.h"
#include "core/timer.h"
#include "arch/arch.h"
#include "spinlock.h"

/* Per-thread producer lock — indexed by TID.
 * Protects head writes for multi-producer safety (IRQ + syscall). */
#define EQ_LOCK_MAX 512
static spinlock_t eq_locks[EQ_LOCK_MAX] = { [0 ... EQ_LOCK_MAX-1] = {0, 0} };

static inline spinlock_t *eq_lock_for(thread_t *t) {
    int idx = t->tid;
    if (idx < 0 || idx >= EQ_LOCK_MAX) idx = 0;
    return &eq_locks[idx];
}

/* ── Post: write event + wake target ─────────── */

void event_post(thread_t *target, uint32_t type, uint64_t data) {
    if (!target) return;
    event_queue_t *eq = &target->eq;

    spinlock_t *lk = eq_lock_for(target);
    uint64_t irqf;
    spin_lock_irq(lk, &irqf);

    uint32_t h = eq->head;
    uint32_t t = eq->tail;

    if (h - t >= EQ_MAX_EVENTS)
        arch_store_release(&eq->tail, t + 1);

    eq->events[h & EQ_MASK].type = type;
    eq->events[h & EQ_MASK].data = data;
    arch_store_release(&eq->head, h + 1);

    spin_unlock_irq(lk, irqf);

    extern void sched_wake(struct thread *t);
    sched_wake(target);
}

/* ── hrtimer timeout callback: wake blocked thread ── */

static void timeout_wake(hrtimer_t *timer) {
    thread_t *t = (thread_t *)timer->data;
    extern void sched_wake(thread_t *t);
    sched_wake(t);
}

/* ── Block: pure timeout sleep (no event queue) ── */

void thread_block_ms(int timeout_ms) {
    if (timeout_ms <= 0) return;

    thread_t *cur = thread_current();
    if (!cur) return;

    if (cur->proc) {
        uint64_t all_pending = cur->proc->sig_pending | cur->sig_thread_pending;
        uint64_t deliverable = all_pending & ~cur->sig_blocked;
        if (deliverable) return;
    }

    /* hrtimer wakes us after timeout */
    hrtimer_t timer;
    hrtimer_init(&timer, timeout_wake, cur);
    hrtimer_start(&timer, hrtimer_now_ns() + ms_to_ns((uint64_t)timeout_ms));

    cur->state = THREAD_BLOCKED;

    extern void schedule(void);
    schedule();

    hrtimer_cancel(&timer);
}

/* ── Wait: consume next event, block if empty ── */

int event_wait(event_queue_t *eq, event_t *out, int timeout_ms) {
    thread_t *cur = thread_current();
    if (!cur) return -14; /* EFAULT */

    /* Set up timeout hrtimer (if finite timeout) */
    hrtimer_t timer;
    int has_timer = 0;
    uint64_t deadline_ns = 0;
    if (timeout_ms > 0) {
        deadline_ns = hrtimer_now_ns() + ms_to_ns((uint64_t)timeout_ms);
        hrtimer_init(&timer, timeout_wake, cur);
        has_timer = 1;
    }

    for (;;) {
        /* Signal check — fatal signals (SIGALRM, SIGKILL) must interrupt.
         * But ignore SIG_DFL-ignored signals (SIGCHLD=17, SIGURG=23,
         * SIGWINCH=28, SIGIO=29) to avoid spurious EINTR. */
        if (cur->proc) {
            uint64_t all_pending = cur->proc->sig_pending | cur->sig_thread_pending;
            uint64_t deliverable = all_pending & ~cur->sig_blocked;
            if (deliverable) {
                /* Filter out SIG_DFL-ignored signals */
                #define SIG_DFL_IGNORE ((1ULL << 16) | (1ULL << 22) | (1ULL << 27) | (1ULL << 28))
                uint64_t real = deliverable;
                for (int s = 1; s < 64 && real; s++) {
                    if (!(real & (1ULL << (s-1)))) continue;
                    if ((uint64_t)cur->proc->sig_actions[s].sa_handler == 0 &&
                        ((1ULL << (s-1)) & SIG_DFL_IGNORE))
                        real &= ~(1ULL << (s-1)); /* SIG_DFL + ignored class */
                }
                if (real) {
                    if (has_timer) hrtimer_cancel(&timer);
                    return -4; /* EINTR */
                }
            }
        }

        /* Fast path: event available */
        uint32_t h = arch_load_acquire(&eq->head);
        uint32_t t = eq->tail;

        if (h != t) {
            *out = eq->events[t & EQ_MASK];
            arch_store_release(&eq->tail, t + 1);
            if (has_timer) hrtimer_cancel(&timer);
            return 0;
        }

        /* Queue empty — non-blocking mode */
        if (timeout_ms == 0)
            return -11; /* EAGAIN */

        /* Check timeout before blocking */
        if (has_timer && hrtimer_now_ns() >= deadline_ns) {
            hrtimer_cancel(&timer);
            return -11; /* EAGAIN (timeout) */
        }

        /* Arm hrtimer for this block iteration */
        if (has_timer)
            hrtimer_start(&timer, deadline_ns);

        cur->state = THREAD_BLOCKED;

        /* Close race: event arrived between fast-path and BLOCKED */
        __asm__ volatile("mfence" ::: "memory");
        if (arch_load_acquire(&eq->head) != eq->tail) {
            cur->state = THREAD_RUNNING;
            continue;
        }

        extern void schedule(void);
        schedule();

        /* Resumed — check timeout */
        if (has_timer && hrtimer_now_ns() >= deadline_ns) {
            hrtimer_cancel(&timer);
            /* Check queue one last time (event might have arrived with timeout) */
            h = arch_load_acquire(&eq->head);
            if (h != eq->tail) {
                *out = eq->events[eq->tail & EQ_MASK];
                arch_store_release(&eq->tail, eq->tail + 1);
                return 0;
            }
            return -11; /* EAGAIN (timeout) */
        }
    }
}
