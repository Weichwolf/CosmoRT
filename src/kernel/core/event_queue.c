/* CosmoRT Event Queue — event_post/event_wait + per-core sleeper lists */

#include "core/event_queue.h"
#include "proc/thread.h"
#include "proc/process.h"
#include "linux/errno.h"
#include "core/percpu.h"
#include "core/timer.h"
#include "core/smp.h"
#include "core/rt.h"
#include "core/nohz.h"
#include "arch/arch.h"
#include "spinlock.h"

#define EQ_LOCK_MAX 512
static spinlock_t eq_locks[EQ_LOCK_MAX] = { [0 ... EQ_LOCK_MAX-1] = {0, 0} };

static inline spinlock_t *eq_lock_for(thread_t *t) {
    int idx = t->tid;
    if (idx < 0 || idx >= EQ_LOCK_MAX) idx = 0;
    return &eq_locks[idx];
}

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

    sched_wake(target);
}

extern void schedule(void);

void thread_block_ms(int timeout_ms) {
    if (timeout_ms <= 0) return;
    thread_t *cur = thread_current();
    if (!cur) return;
    cur->wake_at_tsc = timer_deadline_tsc((uint64_t)timeout_ms);
    cur->blocking_info.type = BLOCK_SLEEP;
    cur->blocking_info.deadline_tsc = cur->wake_at_tsc;
    schedule();
}

int event_wait(event_queue_t *eq, event_t *out, int timeout_ms) {
    uint32_t h = arch_load_acquire(&eq->head);
    uint32_t t = eq->tail;

    if (h != t) {
        *out = eq->events[t & EQ_MASK];
        arch_store_release(&eq->tail, t + 1);
        return 0;
    }

    if (timeout_ms == 0)
        return -EAGAIN;

    thread_t *cur = thread_current();
    if (!cur) return -EFAULT;
    if (cur->proc) {
        uint64_t all_pending = cur->proc->sig_pending | cur->sig_thread_pending;
        uint64_t fatal = all_pending & (SIG_BIT(9) | SIG_BIT(15));
        if (fatal & ~cur->sig_blocked) return -EINTR;
    }

    if (timeout_ms > 0) {
        cur->wake_at_tsc = timer_deadline_tsc((uint64_t)timeout_ms);
    } else {
        cur->wake_at_tsc = 0;
    }
    cur->blocking_info.type = BLOCK_EVENT;
    cur->blocking_info.context = eq;
    cur->blocking_info.deadline_tsc = cur->wake_at_tsc;

    schedule();

    h = arch_load_acquire(&eq->head);
    t = eq->tail;
    if (h != t) {
        *out = eq->events[t & EQ_MASK];
        arch_store_release(&eq->tail, t + 1);
        return 0;
    }

    return -EINTR;
}

#define SLEEPER_MAX 32

static struct {
    thread_t  *threads[SLEEPER_MAX];
    int        count;
    spinlock_t lock;
} core_sleepers[SMP_MAX_CORES] = {
    [0 ... SMP_MAX_CORES-1] = { .lock = SPINLOCK_INIT }
};

static void sleeper_add(thread_t *t) {
    int cpu = percpu_self()->core_id;
    uint64_t irqf;
    spin_lock_irq(&core_sleepers[cpu].lock, &irqf);
    if (core_sleepers[cpu].count < SLEEPER_MAX)
        core_sleepers[cpu].threads[core_sleepers[cpu].count++] = t;
    spin_unlock_irq(&core_sleepers[cpu].lock, irqf);
}

void epoll_sleeper_add_ext(thread_t *t) {
    sleeper_add(t);

    int cpu = percpu_self()->core_id;
    if (nohz_is_tickless(cpu) && t->wake_at_tsc)
        nohz_arm_oneshot(cpu, t->wake_at_tsc);
}

void epoll_wake_all(void) {
    int ncores = smp_num_cores();
    for (int c = 0; c < ncores; c++) {
        uint64_t irqf;
        spin_lock_irq(&core_sleepers[c].lock, &irqf);
        int n = core_sleepers[c].count;
        thread_t *wake[SLEEPER_MAX];
        for (int i = 0; i < n; i++) {
            wake[i] = core_sleepers[c].threads[i];
            core_sleepers[c].threads[i] = 0;
        }
        core_sleepers[c].count = 0;
        spin_unlock_irq(&core_sleepers[c].lock, irqf);

        for (int i = 0; i < n; i++)
            event_post(wake[i], EQ_EPOLL_READY, 0);
    }
}

void epoll_check_timeouts(void) {
    uint64_t now_tsc = timer_tsc_now();
    int cpu = percpu_self()->core_id;

    if (cpu == 0) {
        extern int timerfd_any_expired(void);
        if (timerfd_any_expired()) epoll_wake_all();
    }

    uint64_t irqf;
    spin_lock_irq(&core_sleepers[cpu].lock, &irqf);

    for (int i = 0; i < core_sleepers[cpu].count;) {
        thread_t *t = core_sleepers[cpu].threads[i];
        if (!t) {
            core_sleepers[cpu].threads[i] = core_sleepers[cpu].threads[--core_sleepers[cpu].count];
            continue;
        }
        uint64_t deadline = t->wake_at_tsc;
        if (!deadline && t->wake_at)
            deadline = timer_boot_tsc + t->wake_at * timer_tsc_per_ms;
        if (deadline && now_tsc >= deadline) {
            core_sleepers[cpu].threads[i] = core_sleepers[cpu].threads[--core_sleepers[cpu].count];
            spin_unlock_irq(&core_sleepers[cpu].lock, irqf);
            event_post(t, EQ_TIMEOUT, 0);
            spin_lock_irq(&core_sleepers[cpu].lock, &irqf);
        } else {
            i++;
        }
    }
    spin_unlock_irq(&core_sleepers[cpu].lock, irqf);
}

uint64_t epoll_nearest_deadline_tsc(int core_id) {
    if (core_id < 0 || core_id >= SMP_MAX_CORES) return 0;
    uint64_t nearest = 0;
    uint64_t irqf;
    spin_lock_irq(&core_sleepers[core_id].lock, &irqf);
    for (int i = 0; i < core_sleepers[core_id].count; i++) {
        thread_t *t = core_sleepers[core_id].threads[i];
        if (!t) continue;
        uint64_t dl = t->wake_at_tsc;
        if (!dl && t->wake_at)
            dl = timer_boot_tsc + t->wake_at * timer_tsc_per_ms;
        if (dl && (!nearest || dl < nearest))
            nearest = dl;
    }
    spin_unlock_irq(&core_sleepers[core_id].lock, irqf);
    return nearest;
}
