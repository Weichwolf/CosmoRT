/* CosmoRT Slab Allocator */

#include "slab.h"
#include "memops.h"

void slab_init(slab_t *s, void *pool, int obj_size, int count) {
    s->obj_size = obj_size < 8 ? 8 : obj_size;
    s->capacity = count;
    s->used = 0;
    s->lock = (spinlock_t)SPINLOCK_INIT;

    /* Build free list: each free slot starts with a next pointer */
    s->free_list = 0;
    uint8_t *p = (uint8_t *)pool;
    for (int i = count - 1; i >= 0; i--) {
        void **slot = (void **)(p + (uint64_t)i * s->obj_size);
        *slot = s->free_list;
        s->free_list = slot;
    }
}

void *slab_alloc(slab_t *s) {
    uint64_t flags;
    spin_lock_irq(&s->lock, &flags);

    void *obj = s->free_list;
    if (obj) {
        s->free_list = *(void **)obj;
        s->used++;
        /* Zero the object */
        kmemset(obj, 0, (size_t)s->obj_size);
    }

    spin_unlock_irq(&s->lock, flags);
    return obj;
}

void slab_free(slab_t *s, void *obj) {
    if (!obj) return;

    uint64_t flags;
    spin_lock_irq(&s->lock, &flags);

    *(void **)obj = s->free_list;
    s->free_list = obj;
    s->used--;

    spin_unlock_irq(&s->lock, flags);
}
