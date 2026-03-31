/* CosmoRT Slab Allocator — fixed-size object pools
 *
 * Each pool manages objects of one size. Free list for O(1) alloc/free.
 * Static pools: backed by caller-provided memory (no growth).
 * Dynamic pools: backed by page_alloc, grow on demand.
 */
#ifndef SLAB_H
#define SLAB_H

#include <stdint.h>
#include "spinlock.h"

/* Region tracking for dynamic slabs (linked list of page_alloc'd chunks) */
typedef struct slab_region {
    struct slab_region *next;
    uint8_t *base;
    uint64_t size;      /* bytes */
} slab_region_t;

typedef struct {
    void       *free_list;  /* linked list of free slots */
    uint8_t    *base;       /* base address of first pool region */
    int         obj_size;   /* bytes per object (>= sizeof(void*)) */
    int         capacity;   /* total number of slots across all regions */
    int         used;       /* currently allocated */
    int         dynamic;    /* 1 = can grow via page_alloc */
    slab_region_t *regions; /* linked list of backing regions (dynamic only) */
    spinlock_t  lock;
} slab_t;

/* Initialize a slab pool over pre-allocated memory (static, no growth).
 * pool: base of memory region (must be at least obj_size * count bytes).
 * obj_size: size of each object (minimum 8 bytes for free-list pointer).
 * count: number of objects. */
void slab_init(slab_t *s, void *pool, int obj_size, int count);

/* Initialize a dynamic slab pool that grows via page_alloc on demand.
 * initial_count: objects in initial region (0 = allocate on first use). */
void slab_init_dynamic(slab_t *s, int obj_size, int initial_count);

/* Allocate one object. Returns NULL if pool exhausted (static) or OOM (dynamic).
 * Thread-safe. */
void *slab_alloc(slab_t *s);

/* Free one object back to pool. Thread-safe. */
void slab_free(slab_t *s, void *obj);

#endif
