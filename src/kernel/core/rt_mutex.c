/* CosmoRT RT Mutex — priority-inheritance blocking mutex */

#include "core/rt_mutex.h"
#include "proc/thread.h"
#include "core/percpu.h"
#include "linux/errno.h"
#include "arch/arch.h"

extern void sched_wake(thread_t *t);
extern void sched_add(thread_t *t);
extern int  kernel_setjmp(uint64_t buf[8]);
extern void kernel_longjmp(uint64_t buf[8], int val) __attribute__((noreturn));
extern uint64_t pml4[];

void rt_mutex_init(rt_mutex_t *m) {
    m->owner = 0;
    m->waiter_count = 0;
    m->lock = (spinlock_t)SPINLOCK_INIT;
    for (int i = 0; i < RT_MUTEX_MAX_WAITERS; i++)
        m->waiters[i] = 0;
}

static void pi_boost_owner(thread_t *owner, int waiter_prio) {
    if (!owner || owner->priority >= waiter_prio)
        return;
    if (owner->saved_priority < 0)
        owner->saved_priority = owner->priority;
    __sync_synchronize();
    owner->priority = waiter_prio;
    __sync_synchronize();
}

static void pi_deboost_owner(thread_t *owner) {
    if (!owner || owner->saved_priority < 0)
        return;
    owner->priority = owner->saved_priority;
    owner->saved_priority = -1;
}

static int waiters_insert(rt_mutex_t *m, thread_t *t) {
    if (m->waiter_count >= RT_MUTEX_MAX_WAITERS)
        return -1;

    int prio = t->priority;
    int pos = m->waiter_count;

    for (int i = 0; i < m->waiter_count; i++) {
        if (m->waiters[i]->priority < prio) {
            pos = i;
            break;
        }
    }

    for (int i = m->waiter_count; i > pos; i--)
        m->waiters[i] = m->waiters[i - 1];

    m->waiters[pos] = t;
    m->waiter_count++;
    return 0;
}

#define ADAPTIVE_SPIN_MAX 1000

int rt_mutex_lock(rt_mutex_t *m) {
    thread_t *cur = thread_current();

    for (;;) {
        uint64_t flags;
        spin_lock_irq(&m->lock, &flags);

        if (!m->owner) {
            m->owner = cur;
            spin_unlock_irq(&m->lock, flags);
            return 0;
        }

        if (m->owner == cur) {
            spin_unlock_irq(&m->lock, flags);
            return -EDEADLK;
        }

        thread_t *own = m->owner;
        if (own && own->state == THREAD_RUNNING) {
            spin_unlock_irq(&m->lock, flags);

            for (int i = 0; i < ADAPTIVE_SPIN_MAX; i++) {
                arch_pause();
                if (!m->owner) break;
                if (own->state != THREAD_RUNNING) break;
            }
            continue;
        }

        pi_boost_owner(m->owner, cur->priority);

        int found = 0;
        for (int i = 0; i < m->waiter_count; i++) {
            if (m->waiters[i] == cur) { found = 1; break; }
        }
        if (!found)
            waiters_insert(m, cur);

        spin_unlock_irq(&m->lock, flags);

        if (cur->kthread_fn) {
            for (int i = 0; i < ADAPTIVE_SPIN_MAX; i++)
                arch_pause();
        } else {
            cur->state = THREAD_BLOCKED;
            if (kernel_setjmp(cur->kernel_yield_jmpbuf) == 0) {
                cur->in_kernel_yield = 1;
                arch_set_cr3(virt_to_phys(pml4));
                kernel_longjmp(cur->jmpbuf, 1);
            }
        }
        continue;
    }
}

int rt_mutex_trylock(rt_mutex_t *m) {
    thread_t *cur = thread_current();

    uint64_t flags;
    spin_lock_irq(&m->lock, &flags);

    if (!m->owner) {
        m->owner = cur;
        spin_unlock_irq(&m->lock, flags);
        return 0;
    }

    spin_unlock_irq(&m->lock, flags);
    return -EBUSY;
}

void rt_mutex_unlock(rt_mutex_t *m) {
    uint64_t flags;
    spin_lock_irq(&m->lock, &flags);

    thread_t *owner = m->owner;

    pi_deboost_owner(owner);

    thread_t *new_owner = 0;

    if (m->waiter_count == 0) {
        m->owner = 0;
    } else {
        new_owner = m->waiters[0];
        for (int i = 0; i < m->waiter_count - 1; i++)
            m->waiters[i] = m->waiters[i + 1];
        m->waiter_count--;
        m->waiters[m->waiter_count] = 0;
        m->owner = 0;
    }

    spin_unlock_irq(&m->lock, flags);

    if (new_owner)
        sched_wake(new_owner);
}

int rt_mutex_is_locked(rt_mutex_t *m) {
    return m->owner != 0;
}
