/* CosmoRT Event Queue — kernel-side event_post + event_wait
 *
 * event_post: write event into target thread's queue, then sched_wake.
 * event_wait: consume next event; if empty, block via schedule().
 */

#include "core/event_queue.h"
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

    /* Queue full: advance tail (drop oldest) */
    if (h - t >= EQ_MAX_EVENTS)
        arch_store_release(&eq->tail, t + 1);

    /* Write event at head slot */
    eq->events[h & EQ_MASK].type = type;
    eq->events[h & EQ_MASK].data = data;
    arch_store_release(&eq->head, h + 1);

    spin_unlock_irq(lk, irqf);

    extern void sched_wake(struct thread *t);
    sched_wake(target);
}

/* ── Wait: consume next event, block if empty ── */

extern void epoll_sleeper_add_ext(thread_t *t);

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

    /* Use event_wait with timeout — epoll_check_timeouts posts EQ_TIMEOUT
     * which wakes us through the same path as all other blocking. */
    event_t ev;
    event_wait(&cur->eq, &ev, timeout_ms);
}

int event_wait(event_queue_t *eq, event_t *out, int timeout_ms) {
    thread_t *cur = thread_current();
    if (!cur) return -14; /* EFAULT */
    uint64_t timeout_deadline = (timeout_ms > 0) ? timer_ms() + (uint64_t)timeout_ms : 0;

    for (;;) {
        /* Fast path: event available */
        uint32_t h = arch_load_acquire(&eq->head);
        uint32_t t = eq->tail;  /* only consumer reads tail */

        if (h != t) {
            *out = eq->events[t & EQ_MASK];
            arch_store_release(&eq->tail, t + 1);
            return 0;
        }

        /* Queue empty — non-blocking mode */
        if (timeout_ms == 0)
            return -11; /* EAGAIN */

        /* Check for deliverable signals before blocking */
        if (cur->proc) {
            uint64_t all_pending = cur->proc->sig_pending | cur->sig_thread_pending;
            uint64_t deliverable = all_pending & ~cur->sig_blocked;
            if (deliverable) return -4; /* EINTR */
        }

        /* Set timeout */
        if (timeout_ms > 0) {
            cur->wake_at = timer_ms() + (uint64_t)timeout_ms;
            cur->wake_at_tsc = timer_deadline_tsc((uint64_t)timeout_ms);
        } else {
            cur->wake_at = 0;
            cur->wake_at_tsc = 0;
        }
        epoll_sleeper_add_ext(cur);

        cur->state = THREAD_BLOCKED;

        /* Close race: event_post writes event then sched_wake(CAS BLOCKED→RUNNABLE).
         * If event arrived between our fast-path check and BLOCKED, sched_wake saw
         * RUNNING and was a no-op. Re-check after BLOCKED. */
        __asm__ volatile("mfence" ::: "memory");
        if (arch_load_acquire(&eq->head) != eq->tail) {
            cur->state = THREAD_RUNNING;
            cur->wake_at = 0;
            cur->wake_at_tsc = 0;
            continue;
        }

        extern void schedule(void);
        schedule();

        /* Resumed. Clear wakeup fields, loop back to top.
         * Queue check (fast path) runs first — event may have arrived
         * simultaneously with the signal that woke us (e.g. SIGCHLD + child exit).
         * Signals are checked only if the queue is empty. */
        cur->wake_at = 0;
        cur->wake_at_tsc = 0;

        /* Check timeout (must use original deadline, not cleared wake_at) */
        if (timeout_ms > 0 && timeout_deadline && timer_ms() >= timeout_deadline)
            return -11; /* EAGAIN (timeout) */
    }
}
