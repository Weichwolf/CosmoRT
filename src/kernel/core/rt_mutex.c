/* CosmoRT RT Mutex — Priority-Inheritance Mutex
 *
 * V2: true blocking. Waiters sleep via kernel_setjmp/longjmp and are
 * woken by sched_wake on unlock. The spinlock protects only the
 * owner/waiter metadata (short hold). PI boost/deboost prevents
 * priority inversion.
 */

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

/* ── Init ──────────────────────────────────────── */

void rt_mutex_init(rt_mutex_t *m) {
    m->owner = 0;
    m->waiter_count = 0;
    m->lock = (spinlock_t)SPINLOCK_INIT;
    for (int i = 0; i < RT_MUTEX_MAX_WAITERS; i++)
        m->waiters[i] = 0;
}

/* ── PI boost/deboost ──────────────────────────── */

/* Boost owner to at least waiter's priority.
 * Called with m->lock held. */
static void pi_boost_owner(thread_t *owner, int waiter_prio) {
    if (!owner || owner->priority >= waiter_prio)
        return;
    if (owner->saved_priority < 0)
        owner->saved_priority = owner->priority;
    __sync_synchronize();
    owner->priority = waiter_prio;
    __sync_synchronize();
}

/* Restore owner's original priority after unlock.
 * Called with m->lock held. */
static void pi_deboost_owner(thread_t *owner) {
    if (!owner || owner->saved_priority < 0)
        return;
    owner->priority = owner->saved_priority;
    owner->saved_priority = -1;
}

/* ── Waiter management (sorted insert, highest priority first) ── */

/* Insert waiter into sorted array. Returns 0 on success, -1 if full. */
static int waiters_insert(rt_mutex_t *m, thread_t *t) {
    if (m->waiter_count >= RT_MUTEX_MAX_WAITERS)
        return -1;

    int prio = t->priority;
    int pos = m->waiter_count;

    /* Find insertion point: first slot with lower priority */
    for (int i = 0; i < m->waiter_count; i++) {
        if (m->waiters[i]->priority < prio) {
            pos = i;
            break;
        }
    }

    /* Shift down */
    for (int i = m->waiter_count; i > pos; i--)
        m->waiters[i] = m->waiters[i - 1];

    m->waiters[pos] = t;
    m->waiter_count++;
    return 0;
}

/* ── Lock ──────────────────────────────────────── */

#define ADAPTIVE_SPIN_MAX 1000

int rt_mutex_lock(rt_mutex_t *m) {
    thread_t *cur = thread_current();

    for (;;) {
        uint64_t flags;
        spin_lock_irq(&m->lock, &flags);

        /* Uncontended: acquire */
        if (!m->owner) {
            m->owner = cur;
            spin_unlock_irq(&m->lock, flags);
            return 0;
        }

        /* Deadlock detection: recursive lock */
        if (m->owner == cur) {
            spin_unlock_irq(&m->lock, flags);
            return -EDEADLK;
        }

        /* Adaptive spin: if owner is running on another core, spin briefly
         * instead of blocking. Saves sleep+wake overhead for short critical
         * sections. Must happen BEFORE PI boost. */
        thread_t *own = m->owner;
        if (own && own->state == THREAD_RUNNING) {
            spin_unlock_irq(&m->lock, flags);

            for (int i = 0; i < ADAPTIVE_SPIN_MAX; i++) {
                arch_pause();
                if (!m->owner) break;
                if (own->state != THREAD_RUNNING) break;
            }
            /* Retry from the top — re-acquire spinlock, re-check owner */
            continue;
        }

        /* PI boost: raise owner to our priority if needed */
        pi_boost_owner(m->owner, cur->priority);

        /* Add to waiters (if not already there) */
        int found = 0;
        for (int i = 0; i < m->waiter_count; i++) {
            if (m->waiters[i] == cur) { found = 1; break; }
        }
        if (!found)
            waiters_insert(m, cur);

        /* Block: thread sleeps until woken by rt_mutex_unlock */
        cur->state = THREAD_BLOCKED;
        spin_unlock_irq(&m->lock, flags);

        /* Yield to scheduler via kernel_setjmp/longjmp.
         * kernel_setjmp saves our position in kernel_yield_jmpbuf.
         * kernel_longjmp(jmpbuf,1) returns to thread_run's setjmp,
         * which puts us back in the sched_loop. When sched_wake
         * re-enqueues us, thread_run sees in_kernel_yield and
         * longjmps back here with return value 1. */
        if (kernel_setjmp(cur->kernel_yield_jmpbuf) == 0) {
            cur->in_kernel_yield = 1;
            arch_set_cr3(virt_to_phys(pml4));
            kernel_longjmp(cur->jmpbuf, 1); /* back to sched_loop */
        }
        /* Woken — loop back, re-check if lock is free */
        continue;
    }
}

/* ── Trylock ───────────────────────────────────── */

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

/* ── Unlock ────────────────────────────────────── */

void rt_mutex_unlock(rt_mutex_t *m) {
    uint64_t flags;
    spin_lock_irq(&m->lock, &flags);

    thread_t *owner = m->owner;

    /* PI deboost */
    pi_deboost_owner(owner);

    thread_t *new_owner = 0;

    if (m->waiter_count == 0) {
        /* No waiters: just release */
        m->owner = 0;
    } else {
        /* Hand off to highest-priority waiter (index 0) */
        new_owner = m->waiters[0];
        for (int i = 0; i < m->waiter_count - 1; i++)
            m->waiters[i] = m->waiters[i + 1];
        m->waiter_count--;
        m->waiters[m->waiter_count] = 0;
        /* Don't assign new owner yet — the waiter re-acquires in its loop.
         * Clear owner so the woken thread's CAS succeeds. */
        m->owner = 0;
    }

    spin_unlock_irq(&m->lock, flags);

    /* Wake the highest-priority waiter outside the spinlock */
    if (new_owner)
        sched_wake(new_owner);
}

/* ── Query ─────────────────────────────────────── */

int rt_mutex_is_locked(rt_mutex_t *m) {
    return m->owner != 0;
}
