/* CosmoRT Slab Allocator — fixed-size object pools
 *
 * Each pool manages objects of one size. Free list for O(1) alloc/free.
 * Pool memory is statically allocated (no dynamic growth).
 */
#ifndef SLAB_H
#define SLAB_H

#include <stdint.h>
#include "spinlock.h"

typedef struct {
    void       *free_list;  /* linked list of free slots */
    uint8_t    *base;       /* base address of pool memory */
    int         obj_size;   /* bytes per object (>= sizeof(void*)) */
    int         capacity;   /* total number of slots */
    int         used;       /* currently allocated */
    spinlock_t  lock;
} slab_t;

/* Initialize a slab pool over pre-allocated memory.
 * pool: base of memory region (must be at least obj_size * count bytes).
 * obj_size: size of each object (minimum 8 bytes for free-list pointer).
 * count: number of objects. */
void slab_init(slab_t *s, void *pool, int obj_size, int count);

/* Allocate one object. Returns NULL if pool exhausted. Thread-safe. */
void *slab_alloc(slab_t *s);

/* Free one object back to pool. Thread-safe. */
void slab_free(slab_t *s, void *obj);

#endif
