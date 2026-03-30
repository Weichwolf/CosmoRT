/* CosmoRT Mutex — sleeping lock with Priority Inheritance */
#ifndef MUTEX_H
#define MUTEX_H

#include "core/rt_mutex.h"

typedef struct { rt_mutex_t inner; } mutex_t;

#define MUTEX_INIT { .inner = RT_MUTEX_INIT }

static inline void mutex_init(mutex_t *m)      { rt_mutex_init(&m->inner); }
static inline int  mutex_lock(mutex_t *m)      { return rt_mutex_lock(&m->inner); }
static inline int  mutex_trylock(mutex_t *m)   { return rt_mutex_trylock(&m->inner); }
static inline void mutex_unlock(mutex_t *m)    { rt_mutex_unlock(&m->inner); }
static inline int  mutex_is_locked(mutex_t *m) { return rt_mutex_is_locked(&m->inner); }

#endif
