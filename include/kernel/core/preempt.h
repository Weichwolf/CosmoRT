/* CosmoRT Preemption Control */
#ifndef PREEMPT_H
#define PREEMPT_H

#include "percpu.h"
#include "proc/thread.h"

extern void sched_preempt_voluntary(struct thread *t);

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
        if (t->state == THREAD_RUNNING)
            sched_preempt_voluntary(t);
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
        if (t->state == THREAD_RUNNING)
            sched_preempt_voluntary(t);
    }
}

#endif
