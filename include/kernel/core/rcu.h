/* CosmoRT Preemptible RCU — Read-Copy-Update for lock-free read paths
 *
 * Design follows Linux PREEMPT_RCU:
 * - rcu_read_lock/unlock are preemptible (nesting counter, no IRQ disable)
 * - Grace period waits for all pre-existing readers, including preempted ones
 * - Pointer publish/subscribe: rcu_assign_pointer() / rcu_dereference()
 */
#ifndef RCU_H
#define RCU_H

#include <stdint.h>
#include "arch/arch.h"

/* ── Callback head (embedded in RCU-protected objects) ── */

typedef struct rcu_head {
    struct rcu_head *next;
    void (*func)(struct rcu_head *);
} rcu_head_t;

/* ── Compiler/memory barrier primitives ── */

#define READ_ONCE(x) ({                                      \
    __auto_type __ro_val = *(volatile __typeof__(x) *)&(x);  \
    arch_rmb();                                              \
    __ro_val;                                                \
})

#define WRITE_ONCE(x, v) do {                                \
    *(volatile __typeof__(x) *)&(x) = (v);                  \
    arch_wmb();                                              \
} while (0)

/* Read an RCU-protected pointer. Must be inside rcu_read_lock/unlock. */
#define rcu_dereference(p) ({                                \
    __auto_type ___p = READ_ONCE(p);                         \
    ___p;                                                    \
})

/* Publish a new pointer value. Pairs with rcu_dereference(). */
#define rcu_assign_pointer(p, v) do {                        \
    arch_wmb();                                              \
    WRITE_ONCE(p, v);                                        \
} while (0)

/* ── Core API ── */

void rcu_init(void);
void rcu_read_lock(void);
void rcu_read_unlock(void);
void synchronize_rcu(void);
void call_rcu(rcu_head_t *head, void (*func)(rcu_head_t *));

/* ── Scheduler integration ── */

struct thread;
void rcu_note_context_switch(struct thread *prev);
void rcu_check_callbacks(void);

#endif
