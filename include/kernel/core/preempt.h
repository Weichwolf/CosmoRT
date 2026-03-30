/* CosmoRT Preemption Control */
#ifndef PREEMPT_H
#define PREEMPT_H

#include "percpu.h"
#include "proc/thread.h"

static inline void preempt_disable(void) {
    struct thread *t = thread_current();
    if (t) t->preempt_count++;
}

static inline void preempt_enable(void) {
    struct thread *t = thread_current();
    if (!t) return;
    int cnt = --t->preempt_count;
    if (cnt == 0 && t->need_resched) {
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

static inline int preempt_depth(void) {
    struct thread *t = thread_current();
    return t ? t->preempt_count : 0;
}

static inline int preemptible(void) {
    return preempt_depth() == 0;
}

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
