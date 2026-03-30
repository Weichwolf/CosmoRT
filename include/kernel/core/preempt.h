/* CosmoRT Preemption Control
 *
 * Full Preemption for RT tasks (SCHED_FIFO/RR): immediate reschedule
 * when higher-priority task becomes runnable.
 * Lazy Preemption for Non-RT tasks (SCHED_OTHER): reschedule deferred
 * to voluntary preemption points (syscall-return, schedule(), cond_resched()).
 *
 * preempt_count per thread. >0 = inside critical section, no preemption.
 * need_resched per thread. 1 = reschedule pending.
 */
#ifndef PREEMPT_H
#define PREEMPT_H

#include "percpu.h"
#include "proc/thread.h"

/* Disable preemption (nestable). Call before critical sections. */
static inline void preempt_disable(void) {
    struct thread *t = thread_current();
    if (t) t->preempt_count++;
}

/* Enable preemption. If count drops to 0 and need_resched is set,
 * trigger reschedule via longjmp to sched_loop. */
static inline void preempt_enable(void) {
    struct thread *t = thread_current();
    if (!t) return;
    int cnt = --t->preempt_count;
    if (cnt == 0 && t->need_resched) {
        t->need_resched = 0;
        /* Mark runnable and longjmp to sched_loop.
         * Only for kernel-mode preemption — userspace preemption
         * is handled in sched_preempt via timer IRQ. */
        extern void sched_add(struct thread *t);
        extern void thread_return_to_kernel(struct thread *t);
        if (t->state == THREAD_RUNNING) {
            t->state = THREAD_RUNNABLE;
            sched_add(t);
            thread_return_to_kernel(t);
        }
    }
}

/* Query current preemption depth. 0 = preemptible. */
static inline int preempt_depth(void) {
    struct thread *t = thread_current();
    return t ? t->preempt_count : 0;
}

/* Check if current context is preemptible. */
static inline int preemptible(void) {
    return preempt_depth() == 0;
}

/* Voluntary preemption point for long-running kernel paths.
 * If need_resched is set and preemption is enabled, yield. */
static inline void cond_resched(void) {
    struct thread *t = thread_current();
    if (t && t->preempt_count == 0 && t->need_resched) {
        t->need_resched = 0;
        extern void sched_add(struct thread *t);
        extern void thread_return_to_kernel(struct thread *t);
        if (t->state == THREAD_RUNNING) {
            t->state = THREAD_RUNNABLE;
            sched_add(t);
            thread_return_to_kernel(t);
        }
    }
}

#endif
