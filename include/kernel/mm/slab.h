/* CosmoRT Slab Allocator — fixed-size object pools */
#ifndef SLAB_H
#define SLAB_H

#include <stdint.h>
#include "spinlock.h"

typedef struct slab_region {
    struct slab_region *next;
    uint8_t *base;
    uint64_t size;
} slab_region_t;

typedef struct {
    void       *free_list;
    uint8_t    *base;
    int         obj_size;
    int         capacity;
    int         used;
    int         dynamic;
    slab_region_t *regions;
    spinlock_t  lock;
} slab_t;

void slab_init(slab_t *s, void *pool, int obj_size, int count);

void slab_init_dynamic(slab_t *s, int obj_size, int initial_count);

void *slab_alloc(slab_t *s);

void slab_free(slab_t *s, void *obj);

#endif
