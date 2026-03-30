/* CosmoRT Read-Copy-Update */
#ifndef RCU_H
#define RCU_H

#include "core/preempt.h"

struct rcu_head {
    struct rcu_head *next;
    void (*func)(struct rcu_head *);
};

static inline void rcu_read_lock(void) { preempt_disable(); }
static inline void rcu_read_unlock(void) { preempt_enable(); }

void synchronize_rcu(void);
void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *));
void rcu_check_quiescent(void);

#endif
