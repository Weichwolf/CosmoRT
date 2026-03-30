/* CosmoRT RT Mutex — Priority-Inheritance Mutex
 *
 * Sleeping mutex with PI for kernel use. When a high-priority thread
 * blocks on a mutex held by a low-priority thread, the owner is
 * temporarily boosted to prevent priority inversion.
 *
 * V2: true blocking via kernel_setjmp/longjmp. Waiters sleep in the
 * scheduler and are woken by sched_wake on unlock. Adaptive spin
 * for owners running on other cores.
 *
 * Waiters array sorted by priority (highest first). Fixed capacity
 * sufficient for kernel-internal use (not exposed to userspace).
 */
#ifndef RT_MUTEX_H
#define RT_MUTEX_H

#include <stdint.h>
#include "spinlock.h"

typedef struct thread thread_t;

#define RT_MUTEX_MAX_WAITERS 8

typedef struct rt_mutex {
    thread_t   *owner;                          /* NULL = unlocked */
    thread_t   *waiters[RT_MUTEX_MAX_WAITERS];  /* sorted by priority, highest first */
    int         waiter_count;
    spinlock_t  lock;                           /* protects owner + waiters */
} rt_mutex_t;

#define RT_MUTEX_INIT { .owner = 0, .waiter_count = 0, .lock = SPINLOCK_INIT }

void rt_mutex_init(rt_mutex_t *m);
int  rt_mutex_lock(rt_mutex_t *m);      /* blocks until free, returns 0 */
int  rt_mutex_trylock(rt_mutex_t *m);   /* returns 0 on success, -EBUSY */
void rt_mutex_unlock(rt_mutex_t *m);
int  rt_mutex_is_locked(rt_mutex_t *m);

#endif
