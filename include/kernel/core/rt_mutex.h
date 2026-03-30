/* CosmoRT RT Mutex — Priority-Inheritance Mutex */
#ifndef RT_MUTEX_H
#define RT_MUTEX_H

#include <stdint.h>
#include "spinlock.h"

typedef struct thread thread_t;

#define RT_MUTEX_MAX_WAITERS 8

typedef struct rt_mutex {
    thread_t   *owner;
    thread_t   *waiters[RT_MUTEX_MAX_WAITERS];
    int         waiter_count;
    spinlock_t  lock;
} rt_mutex_t;

#define RT_MUTEX_INIT { .owner = 0, .waiter_count = 0, .lock = SPINLOCK_INIT }

void rt_mutex_init(rt_mutex_t *m);
int  rt_mutex_lock(rt_mutex_t *m);
int  rt_mutex_trylock(rt_mutex_t *m);
void rt_mutex_unlock(rt_mutex_t *m);
int  rt_mutex_is_locked(rt_mutex_t *m);

#endif
