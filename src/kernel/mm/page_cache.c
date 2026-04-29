/* CosmoRT Page Cache — (inode, offset) → physical page hash table
 *
 * Hash table with chaining. Entries are slab-allocated.
 * Protected by a single spinlock; I/O happens outside the lock. */

#include "mm/page_cache.h"
#include "mm/slab.h"
#include "mm/page_alloc.h"
#include "config.h"
#include "spinlock.h"
#include "fs/vfs.h"
#include "memops.h"

/* Defined in src/kernel/proc/process.c — refcount-aware page allocator,
 * returns a page with refcount=1 (vs. page_alloc which returns refcount=0). */
extern uint64_t *alloc_page(void);

#define PC_HASH_SIZE 4096  /* must be power of 2 — 4K buckets for low collision */

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

/* Stats — exposed via /proc/cosmort/page_cache. Atomic to be SMP-safe
 * without paying lock-cost on every accounting bump. */
volatile uint64_t pc_stat_hit, pc_stat_miss, pc_stat_ra, pc_stat_anon;

static inline uint32_t pc_hash_fn(uint64_t ino, uint64_t offset) {
    uint64_t h = ino * 2654435761ULL + (offset >> 12) * 2246822519ULL;
    return (uint32_t)(h & (PC_HASH_SIZE - 1));
}

static void pc_ensure_slab(void) {
    if (__builtin_expect(pc_slab_ready, 1)) return;
    slab_init_dynamic(&pc_slab, sizeof(pc_entry_t), 64);
    pc_slab_ready = 1;
}

/* Lookup helper, called with lock held. */
static pc_entry_t *pc_find_locked(uint64_t ino, uint64_t offset) {
    uint32_t idx = pc_hash_fn(ino, offset);
    for (pc_entry_t *e = pc_hash[idx]; e; e = e->next)
        if (e->ino == ino && e->offset == offset)
            return e;
    return 0;
}

uint64_t page_cache_lookup(uint64_t ino, uint64_t offset) {
    uint64_t irqf;
    spin_lock_irq(&pc_lock, &irqf);
    pc_entry_t *e = pc_find_locked(ino, offset);
    uint64_t phys = e ? e->phys : 0;
    spin_unlock_irq(&pc_lock, irqf);
    return phys;
}

void page_cache_insert(uint64_t ino, uint64_t offset, uint64_t phys) {
    pc_ensure_slab();
    pc_entry_t *e = slab_alloc(&pc_slab);
    if (!e) return;
    e->ino = ino;
    e->offset = offset;
    e->phys = phys;

    /* Cache owns a reference so the page survives when all PTEs are gone */
    page_incref(phys);

    uint64_t irqf;
    spin_lock_irq(&pc_lock, &irqf);
    /* Defensive: if a concurrent inserter beat us to it, drop ours */
    pc_entry_t *existing = pc_find_locked(ino, offset);
    if (existing) {
        spin_unlock_irq(&pc_lock, irqf);
        slab_free(&pc_slab, e);
        page_free(phys_to_virt(phys));  /* releases the ref we just added */
        return;
    }
    uint32_t idx = pc_hash_fn(ino, offset);
    e->next = pc_hash[idx];
    pc_hash[idx] = e;
    spin_unlock_irq(&pc_lock, irqf);
}

/* Atomic get-or-load.
 *
 * Caller wants a physical page for (ino, offset). Returns the phys with
 * refcount already incremented for the caller's PTE. The cache holds a
 * separate ref so the page survives when no PTE references it.
 *
 * Race model: the only invariants we need are
 *   1. Hot loaders don't double-allocate physical pages forever
 *   2. The returned phys is alive (refcount >= 1 for caller)
 *   3. The cache entry, if any, points at a phys with cache-ref >= 1
 *
 * We accept a brief race window where two threads both miss, both call
 * vfs_pread, then re-acquire the lock and one inserts; the loser
 * page_free's its tentative page and returns the winner's. This costs
 * at worst one duplicated I/O per concurrent miss — much cheaper than
 * holding a global lock across blocking I/O. */
uint64_t page_cache_get_or_load(int backend, uint64_t ino, uint64_t offset) {
    pc_ensure_slab();

    /* Fast path: hit. */
    {
        uint64_t irqf;
        spin_lock_irq(&pc_lock, &irqf);
        pc_entry_t *e = pc_find_locked(ino, offset);
        if (e) {
            uint64_t phys = e->phys;
            page_incref(phys);  /* caller's ref */
            spin_unlock_irq(&pc_lock, irqf);
            __atomic_fetch_add(&pc_stat_hit, 1, __ATOMIC_RELAXED);
            return phys;
        }
        spin_unlock_irq(&pc_lock, irqf);
    }
    __atomic_fetch_add(&pc_stat_miss, 1, __ATOMIC_RELAXED);

    /* Miss: alloc + load outside lock. */
    uint64_t *pg = alloc_page();
    if (!pg) return 0;

    long rd = vfs_pread_by_ino(backend, ino, pg, (size_t)offset, 4096);
    if (rd < 0) {
        page_free(pg);
        return 0;
    }
    /* Short read: zero the tail (sparse / EOF). alloc_page does not
     * necessarily zero — be defensive. */
    if (rd < 4096)
        kmemset((uint8_t *)pg + rd, 0, 4096 - (size_t)rd);

    uint64_t our_phys = virt_to_phys(pg);

    /* Re-acquire lock; check for racing winner. */
    pc_entry_t *e = slab_alloc(&pc_slab);
    if (!e) {
        /* Slab OOM: still safe to return our page, just bypass cache. */
        page_incref(our_phys);  /* caller ref (we already hold one from alloc_page) */
        page_free(pg);          /* drop alloc_page's ref; caller_ref keeps it alive */
        return our_phys;
    }
    e->ino = ino;
    e->offset = offset;
    e->phys = our_phys;

    uint64_t irqf;
    spin_lock_irq(&pc_lock, &irqf);
    pc_entry_t *winner = pc_find_locked(ino, offset);
    if (winner) {
        /* Someone else inserted while we loaded. Use theirs. */
        uint64_t winner_phys = winner->phys;
        page_incref(winner_phys);  /* caller's ref on winner */
        spin_unlock_irq(&pc_lock, irqf);
        slab_free(&pc_slab, e);
        page_free(pg);  /* drop our tentative — was never inserted */
        return winner_phys;
    }
    /* We win: cache takes a ref (page_incref), caller already has one
     * via alloc_page's initial refcount=1. */
    page_incref(our_phys);  /* cache ref (alloc_page gave caller 1) */
    uint32_t idx = pc_hash_fn(ino, offset);
    e->next = pc_hash[idx];
    pc_hash[idx] = e;
    spin_unlock_irq(&pc_lock, irqf);
    return our_phys;
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
            uint64_t phys = e->phys;
            spin_unlock_irq(&pc_lock, irqf);
            slab_free(&pc_slab, e);
            page_free(phys_to_virt(phys));
            return;
        }
        pp = &e->next;
    }
    spin_unlock_irq(&pc_lock, irqf);
}

void page_cache_invalidate_ino(uint64_t ino) {
    if (!pc_slab_ready) return;
    /* Collect victims under lock, release refs outside to avoid reentrancy */
    pc_entry_t *victims = 0;
    uint64_t irqf;
    spin_lock_irq(&pc_lock, &irqf);
    for (uint32_t i = 0; i < PC_HASH_SIZE; i++) {
        pc_entry_t **pp = &pc_hash[i];
        while (*pp) {
            pc_entry_t *e = *pp;
            if (e->ino == ino) {
                *pp = e->next;
                e->next = victims;
                victims = e;
            } else {
                pp = &e->next;
            }
        }
    }
    spin_unlock_irq(&pc_lock, irqf);
    while (victims) {
        pc_entry_t *e = victims;
        victims = e->next;
        uint64_t phys = e->phys;
        slab_free(&pc_slab, e);
        page_free(phys_to_virt(phys));
    }
}
