/* CosmoRT Slab Allocator */

#include "mm/slab.h"
#include "memops.h"
#include "hw/serial.h"

void slab_init(slab_t *s, void *pool, int obj_size, int count) {
    s->obj_size = obj_size < 8 ? 8 : obj_size;
    s->capacity = count;
    s->used = 0;
    s->base = (uint8_t *)pool;
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

    /* Validate obj belongs to this slab's pool and is aligned */
    uint8_t *p = (uint8_t *)obj;
    uint64_t pool_size = (uint64_t)s->capacity * s->obj_size;
    if (p < s->base || p >= s->base + pool_size ||
        (uint64_t)(p - s->base) % (uint64_t)s->obj_size != 0) {
        serial_puts("slab_free: invalid pointer\n");
        return;
    }

    uint64_t flags;
    spin_lock_irq(&s->lock, &flags);

    *(void **)obj = s->free_list;
    s->free_list = obj;
    s->used--;

    spin_unlock_irq(&s->lock, flags);
}
