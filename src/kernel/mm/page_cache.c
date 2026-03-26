/* CosmoRT Page Cache — (inode, offset) → physical page hash table
 *
 * Hash table with chaining. Entries are slab-allocated.
 * Protected by a single spinlock (sufficient for current single-RT-core I/O). */

#include "mm/page_cache.h"
#include "mm/slab.h"
#include "spinlock.h"

#define PC_HASH_SIZE 1024  /* must be power of 2 */

typedef struct pc_entry {
    uint64_t ino;
    uint64_t offset;
    uint64_t phys;
    struct pc_entry *next;
} pc_entry_t;

static pc_entry_t *pc_hash[PC_HASH_SIZE];
static spinlock_t pc_lock = SPINLOCK_INIT;
static slab_t pc_slab;
static int pc_slab_ready;

static inline uint32_t pc_hash_fn(uint64_t ino, uint64_t offset) {
    uint64_t h = ino * 2654435761ULL + (offset >> 12) * 2246822519ULL;
    return (uint32_t)(h & (PC_HASH_SIZE - 1));
}

static void pc_ensure_slab(void) {
    if (__builtin_expect(pc_slab_ready, 1)) return;
    slab_init_dynamic(&pc_slab, sizeof(pc_entry_t), 64);
    pc_slab_ready = 1;
}

uint64_t page_cache_lookup(uint64_t ino, uint64_t offset) {
    uint64_t irqf;
    spin_lock_irq(&pc_lock, &irqf);
    uint32_t idx = pc_hash_fn(ino, offset);
    for (pc_entry_t *e = pc_hash[idx]; e; e = e->next) {
        if (e->ino == ino && e->offset == offset) {
            uint64_t phys = e->phys;
            spin_unlock_irq(&pc_lock, irqf);
            return phys;
        }
    }
    spin_unlock_irq(&pc_lock, irqf);
    return 0;
}

void page_cache_insert(uint64_t ino, uint64_t offset, uint64_t phys) {
    pc_ensure_slab();
    pc_entry_t *e = slab_alloc(&pc_slab);
    if (!e) return;
    e->ino = ino;
    e->offset = offset;
    e->phys = phys;

    uint64_t irqf;
    spin_lock_irq(&pc_lock, &irqf);
    uint32_t idx = pc_hash_fn(ino, offset);
    e->next = pc_hash[idx];
    pc_hash[idx] = e;
    spin_unlock_irq(&pc_lock, irqf);
}

void page_cache_remove(uint64_t ino, uint64_t offset) {
    uint64_t irqf;
    spin_lock_irq(&pc_lock, &irqf);
    uint32_t idx = pc_hash_fn(ino, offset);
    pc_entry_t **pp = &pc_hash[idx];
    while (*pp) {
        pc_entry_t *e = *pp;
        if (e->ino == ino && e->offset == offset) {
            *pp = e->next;
            spin_unlock_irq(&pc_lock, irqf);
            slab_free(&pc_slab, e);
            return;
        }
        pp = &e->next;
    }
    spin_unlock_irq(&pc_lock, irqf);
}

void page_cache_evict(uint64_t phys) {
    if (!pc_slab_ready) return;
    uint64_t irqf;
    spin_lock_irq(&pc_lock, &irqf);
    for (uint32_t i = 0; i < PC_HASH_SIZE; i++) {
        pc_entry_t **pp = &pc_hash[i];
        while (*pp) {
            pc_entry_t *e = *pp;
            if (e->phys == phys) {
                *pp = e->next;
                spin_unlock_irq(&pc_lock, irqf);
                slab_free(&pc_slab, e);
                return; /* one phys → one entry */
            }
            pp = &e->next;
        }
    }
    spin_unlock_irq(&pc_lock, irqf);
}
