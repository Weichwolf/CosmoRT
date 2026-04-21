/* CosmoRT Slab Allocator */

#include "mm/slab.h"
#include "mm/page_alloc.h"
#include "memops.h"
#include "hw/serial.h"

/* Build free list over a contiguous region */
static void build_free_list(slab_t *s, uint8_t *base, int count) {
    for (int i = count - 1; i >= 0; i--) {
        void **slot = (void **)(base + (uint64_t)i * s->obj_size);
        *slot = s->free_list;
        s->free_list = slot;
    }
    s->capacity += count;
}

void slab_init(slab_t *s, void *pool, int obj_size, int count) {
    s->obj_size = obj_size < 8 ? 8 : obj_size;
    s->capacity = 0;
    s->used = 0;
    s->base = (uint8_t *)pool;
    s->dynamic = 0;
    s->regions = 0;
    s->lock = (spinlock_t)SPINLOCK_INIT;
    s->free_list = 0;
    build_free_list(s, (uint8_t *)pool, count);
}

/* Grow a dynamic slab by allocating pages. Returns count added, 0 on OOM.
 * Caller must hold s->lock.
 *
 * pages_alloc is buddy-backed and capped at MAX_ORDER (currently 9 → 512
 * pages = 2 MB). For huge objects (e.g. UDP sockets with embedded packet
 * queues) the naive "16 objects per region" target would exceed that cap.
 * The strategy is therefore: pick the largest power-of-two region size
 * that the buddy can serve, then derive the actual object count from it. */
#define SLAB_GROW_TARGET_OBJS  16

static int slab_grow_locked(slab_t *s) {
    int objs_per_page = 4096 / s->obj_size;
    if (objs_per_page < 1) objs_per_page = 1;
    int batch = objs_per_page * 4;
    if (batch < SLAB_GROW_TARGET_OBJS) batch = SLAB_GROW_TARGET_OBJS;

    uint64_t bytes = (uint64_t)batch * s->obj_size;
    int npages = (int)((bytes + 4095) / 4096);
    if (npages < 1) npages = 1;
    if (npages > PAGES_ALLOC_MAX) npages = PAGES_ALLOC_MAX;

    int alloc_pages = 1;
    while (alloc_pages < npages) alloc_pages <<= 1;

    uint8_t *mem = (uint8_t *)pages_alloc(alloc_pages);
    if (!mem) return 0;

    uint64_t region_bytes = (uint64_t)alloc_pages * 4096;
    int actual_count = (int)(region_bytes / (uint64_t)s->obj_size);
    if (actual_count < 1) { pages_free(mem, alloc_pages); return 0; }

    /* Track region — use first obj_size bytes as region header if obj_size >= sizeof(slab_region_t),
     * otherwise allocate a page for bookkeeping. For simplicity, embed in the region list
     * using the first slot. */
    /* We store region metadata in a small static-ish way: use page_alloc for a region node.
     * slab_region_t is 24 bytes, fits in any object. But we can't use a slab slot because
     * we need it for actual objects. Use the first few bytes of the allocated region
     * if obj_size allows, but that wastes a slot. Better: embed in a simple linked list
     * allocated from the region itself — sacrifice first slot for bookkeeping. */
    slab_region_t *reg = (slab_region_t *)mem;
    reg->base = mem;
    reg->size = region_bytes;
    reg->next = s->regions;
    s->regions = reg;

    /* Build free list from slot 1 onwards (slot 0 is the region header) */
    int usable = actual_count - 1;
    if (usable <= 0) { pages_free(mem, alloc_pages); s->regions = reg->next; return 0; }

    uint8_t *first_obj = mem + (uint64_t)s->obj_size;
    build_free_list(s, first_obj, usable);

    return usable;
}

void slab_init_dynamic(slab_t *s, int obj_size, int initial_count) {
    s->obj_size = obj_size < 8 ? 8 : obj_size;
    /* Ensure obj_size >= sizeof(slab_region_t) so region header fits in one slot */
    if (s->obj_size < (int)sizeof(slab_region_t))
        s->obj_size = (int)sizeof(slab_region_t);
    s->capacity = 0;
    s->used = 0;
    s->base = 0;
    s->dynamic = 1;
    s->regions = 0;
    s->lock = (spinlock_t)SPINLOCK_INIT;
    s->free_list = 0;

    if (initial_count > 0)
        slab_grow_locked(s); /* no lock needed, not yet shared */
}

void *slab_alloc(slab_t *s) {
    uint64_t flags;
    spin_lock_irq(&s->lock, &flags);

    /* Grow if empty and dynamic */
    if (!s->free_list && s->dynamic) {
        if (slab_grow_locked(s) == 0) {
            spin_unlock_irq(&s->lock, flags);
            return 0; /* OOM */
        }
    }

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

    if (s->dynamic) {
        /* Validate: obj must belong to one of the regions */
        int valid = 0;
        for (slab_region_t *r = s->regions; r; r = r->next) {
            uint8_t *p = (uint8_t *)obj;
            /* Objects start at r->base + obj_size (slot 0 is region header) */
            uint8_t *first = r->base + s->obj_size;
            uint8_t *end = r->base + r->size;
            if (p >= first && p < end &&
                (uint64_t)(p - first) % (uint64_t)s->obj_size == 0) {
                valid = 1;
                break;
            }
        }
        if (!valid) {
            serial_puts("slab_free: invalid pointer (dynamic)\n");
            return;
        }
    } else {
        /* Static slab validation */
        uint8_t *p = (uint8_t *)obj;
        uint64_t pool_size = (uint64_t)s->capacity * s->obj_size;
        if (p < s->base || p >= s->base + pool_size ||
            (uint64_t)(p - s->base) % (uint64_t)s->obj_size != 0) {
            serial_puts("slab_free: invalid pointer\n");
            return;
        }
    }

    uint64_t flags;
    spin_lock_irq(&s->lock, &flags);

    *(void **)obj = s->free_list;
    s->free_list = obj;
    s->used--;

    spin_unlock_irq(&s->lock, flags);
}
