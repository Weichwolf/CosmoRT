/* CosmoRT Read-Copy-Update */

#include "core/rcu.h"
#include "core/smp.h"
#include "core/percpu.h"
#include "core/timer.h"
#include "spinlock.h"
#include "config.h"

#define RCU_SYNC_TIMEOUT_MS 5000

static volatile uint64_t rcu_gp_seq;
static volatile uint64_t rcu_cpu_seq[SMP_MAX_CORES];

static struct rcu_head *rcu_cb_head;
static struct rcu_head *rcu_cb_tail;
static uint64_t rcu_cb_target_gp;
static spinlock_t rcu_cb_lock = SPINLOCK_INIT;

void rcu_check_quiescent(void) {
    int cpu = percpu_self()->core_id;
    __atomic_store_n(&rcu_cpu_seq[cpu], __atomic_load_n(&rcu_gp_seq, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);

    struct rcu_head *run_list = 0;
    uint64_t gp = __atomic_load_n(&rcu_gp_seq, __ATOMIC_ACQUIRE);

    if (!rcu_cb_head)
        return;

    int ncores = smp_num_cores();
    for (int c = 0; c < ncores; c++) {
        if (__atomic_load_n(&rcu_cpu_seq[c], __ATOMIC_ACQUIRE) < gp)
            return;
    }

    uint64_t flags;
    spin_lock_irq(&rcu_cb_lock, &flags);
    if (rcu_cb_head && rcu_cb_target_gp <= gp) {
        run_list = rcu_cb_head;
        rcu_cb_head = 0;
        rcu_cb_tail = 0;
    }
    spin_unlock_irq(&rcu_cb_lock, flags);

    while (run_list) {
        struct rcu_head *next = run_list->next;
        run_list->func(run_list);
        run_list = next;
    }
}

void synchronize_rcu(void) {
    uint64_t target = __sync_add_and_fetch(&rcu_gp_seq, 1);
    int ncores = smp_num_cores();
    uint64_t deadline = timer_ms() + RCU_SYNC_TIMEOUT_MS;

    for (;;) {
        int done = 1;
        for (int c = 0; c < ncores; c++) {
            if (__atomic_load_n(&rcu_cpu_seq[c], __ATOMIC_ACQUIRE) < target) {
                done = 0;
                break;
            }
        }
        if (done)
            return;
        if (timer_ms() > deadline)
            return;
        __asm__ volatile("pause" ::: "memory");
    }
}

void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *)) {
    head->func = func;
    head->next = 0;

    uint64_t target = __sync_add_and_fetch(&rcu_gp_seq, 1);

    uint64_t flags;
    spin_lock_irq(&rcu_cb_lock, &flags);
    if (rcu_cb_tail)
        rcu_cb_tail->next = head;
    else
        rcu_cb_head = head;
    rcu_cb_tail = head;
    rcu_cb_target_gp = target;
    spin_unlock_irq(&rcu_cb_lock, flags);
}
