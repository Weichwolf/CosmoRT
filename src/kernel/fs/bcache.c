/* CosmoRT Block Cache — LRU with hash table lookup
 *
 * 4MB cache (1024 × 4KB), 1024 hash buckets, doubly-linked LRU with O(1)
 * head/tail pointers. Supports synchronous bulk read-ahead via virtio-blk
 * bulk-read API: contiguous block ranges get fetched in a single virtio
 * request (one IRQ, one memcpy out of the DMA buffer).
 */

#include "fs/bcache.h"
#include "mm/page_alloc.h"
#include "memops.h"
#include "hw/serial.h"
#include "spinlock.h"

/* Block driver (virtio-blk) */
extern int blk_read(uint64_t block, void *buf);
extern int blk_write(uint64_t block, const void *buf);
extern int blk_read_bulk(uint64_t start_block, uint32_t count, void *buf);
extern uint32_t blk_bulk_max(void);

/* ── Hash table ───────────────────────────────────── */

/* 1024 entries / 1024 buckets = avg 1.0 entries per bucket — O(1) lookup.
 * Multiplicative hash (Knuth) spreads sequential block numbers. */
#define HASH_BUCKETS 1024
#define HASH(b) ((uint32_t)((b) * 2654435761u) & (HASH_BUCKETS - 1))

static struct bcache_entry entries[BCACHE_SIZE];
static struct bcache_entry *hash_table[HASH_BUCKETS];

/* LRU list: lru_head.lru_next = MRU, lru_tail = LRU (or NULL when empty). */
static struct bcache_entry lru_head;
static struct bcache_entry *lru_tail;
static spinlock_t cache_lock = SPINLOCK_INIT;

/* ── LRU helpers ──────────────────────────────────── */

static void lru_remove(struct bcache_entry *e) {
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    if (lru_tail == e) lru_tail = e->lru_prev;
    if (lru_tail == &lru_head) lru_tail = 0;
    e->lru_prev = e->lru_next = 0;
}

static void lru_push_front(struct bcache_entry *e) {
    e->lru_prev = &lru_head;
    e->lru_next = lru_head.lru_next;
    if (lru_head.lru_next) lru_head.lru_next->lru_prev = e;
    else                   lru_tail = e;
    lru_head.lru_next = e;
}

/* ── Hash helpers ─────────────────────────────────── */

static void hash_insert(struct bcache_entry *e) {
    uint32_t h = HASH(e->block_nr);
    e->hash_next = hash_table[h];
    hash_table[h] = e;
}

static void hash_remove(struct bcache_entry *e) {
    uint32_t h = HASH(e->block_nr);
    struct bcache_entry **pp = &hash_table[h];
    while (*pp) {
        if (*pp == e) { *pp = e->hash_next; e->hash_next = 0; return; }
        pp = &(*pp)->hash_next;
    }
}

static struct bcache_entry *hash_find(uint64_t block) {
    uint32_t h = HASH(block);
    struct bcache_entry *e = hash_table[h];
    while (e) {
        if (e->block_nr == block) return e;
        e = e->hash_next;
    }
    return 0;
}

/* ── Init ─────────────────────────────────────────── */

void bcache_init(void) {
    lru_head.lru_next = 0;
    lru_head.lru_prev = 0;
    lru_tail = 0;

    for (int i = 0; i < BCACHE_SIZE; i++) {
        entries[i].block_nr = BCACHE_INVALID;
        entries[i].data = (uint8_t *)page_alloc();
        entries[i].dirty = 0;
        entries[i].refcount = 0;
        entries[i].hash_next = 0;
        entries[i].lru_prev = entries[i].lru_next = 0;
        if (!entries[i].data) {
            serial_puts("bcache: page_alloc failed\n");
            return;
        }
        /* All entries start on LRU (free) */
        lru_push_front(&entries[i]);
    }

    for (int i = 0; i < HASH_BUCKETS; i++)
        hash_table[i] = 0;

    serial_puts("bcache: init (");
    { char t[8]; int ti=0; int v=BCACHE_SIZE;
      do{t[ti++]='0'+(char)(v%10);v/=10;}while(v);
      while(ti--) serial_putchar(t[ti]); }
    serial_puts(" blocks, ");
    { char t[8]; int ti=0; int v=BCACHE_SIZE * 4 / 1024;
      do{t[ti++]='0'+(char)(v%10);v/=10;}while(v);
      while(ti--) serial_putchar(t[ti]); }
    serial_puts(" MB)\n");
}

/* ── Evict LRU entry ─────────────────────────────── */
/* Walks tail backwards looking for an unpinned entry. Caller holds cache_lock. */
static struct bcache_entry *evict_one(void) {
    struct bcache_entry *e = lru_tail;
    while (e && e != &lru_head) {
        if (e->refcount == 0) {
            if (e->dirty && e->block_nr != BCACHE_INVALID) {
                blk_write(e->block_nr, e->data);
                e->dirty = 0;
            }
            if (e->block_nr != BCACHE_INVALID)
                hash_remove(e);
            lru_remove(e);
            e->block_nr = BCACHE_INVALID;
            return e;
        }
        e = e->lru_prev;
    }
    serial_puts("bcache: all entries pinned!\n");
    return 0;
}

/* ── Get (read + pin) ─────────────────────────────── */

struct bcache_entry *bcache_get(uint64_t block) {
    uint64_t flags;
    spin_lock_irq(&cache_lock, &flags);

    /* Check cache */
    struct bcache_entry *e = hash_find(block);
    if (e) {
        e->refcount++;
        /* Move to front of LRU */
        lru_remove(e);
        lru_push_front(e);
        spin_unlock_irq(&cache_lock, flags);
        return e;
    }

    /* Cache miss — evict and read */
    e = evict_one();
    if (!e) {
        spin_unlock_irq(&cache_lock, flags);
        return 0;
    }

    e->block_nr = block;
    e->dirty = 0;
    e->refcount = 1;
    hash_insert(e);
    lru_push_front(e);

    /* Read from disk (drop lock during I/O) */
    spin_unlock_irq(&cache_lock, flags);

    if (blk_read(block, e->data) < 0) {
        spin_lock_irq(&cache_lock, &flags);
        hash_remove(e);
        lru_remove(e);
        e->block_nr = BCACHE_INVALID;
        e->refcount = 0;
        lru_push_front(e);
        spin_unlock_irq(&cache_lock, flags);
        return 0;
    }

    return e;
}

/* ── Put (unpin) ──────────────────────────────────── */

void bcache_put(struct bcache_entry *e) {
    if (!e) return;
    uint64_t flags;
    spin_lock_irq(&cache_lock, &flags);
    if (e->refcount > 0) e->refcount--;
    spin_unlock_irq(&cache_lock, flags);
}

/* ── Mark dirty ───────────────────────────────────── */

void bcache_mark_dirty(struct bcache_entry *e) {
    if (e) e->dirty = 1;
}

/* ── Sync all dirty ───────────────────────────────── */

void bcache_sync(void) {
    uint64_t flags;
    spin_lock_irq(&cache_lock, &flags);

    for (int i = 0; i < BCACHE_SIZE; i++) {
        if (entries[i].dirty && entries[i].block_nr != BCACHE_INVALID) {
            blk_write(entries[i].block_nr, entries[i].data);
            entries[i].dirty = 0;
        }
    }

    spin_unlock_irq(&cache_lock, flags);
}

/* ── Convenience: write through cache ─────────────── */

int bcache_write_block(uint64_t block, const void *data) {
    struct bcache_entry *e = bcache_get(block);
    if (!e) return -1;
    kmemcpy(e->data, data, 4096);
    bcache_mark_dirty(e);
    bcache_put(e);
    return 0;
}

/* ── Read-ahead ───────────────────────────────────── */
/* Prefetch up to `count` contiguous blocks starting at `start`.
 * Fully cached ranges are skipped. Missing runs (1..bulk_max blocks) are
 * fetched in single virtio bulk requests, then dispatched into the cache.
 * Prefetched entries are NOT pinned (refcount stays 0) — they are just hot.
 *
 * Caller must NOT hold cache_lock. */
void bcache_readahead(uint64_t start, uint32_t count) {
    if (count == 0) return;
    uint32_t bmax = blk_bulk_max();
    if (bmax == 0) bmax = 1;

    /* Static landing buffer for bulk DMA → cache copy. 64KB (16 blocks).
     * Read-ahead happens under fs_lock paths, never re-entered while in
     * progress on the same CPU; protect against parallel CPUs with the
     * cache_lock window during dispatch. */
    static uint8_t bulk_buf[16 * 4096] __attribute__((aligned(4096)));
    if (bmax > 16) bmax = 16;

    uint32_t i = 0;
    while (i < count) {
        /* Find next missing block */
        uint64_t flags;
        spin_lock_irq(&cache_lock, &flags);
        while (i < count && hash_find(start + i)) i++;
        if (i >= count) { spin_unlock_irq(&cache_lock, flags); return; }
        uint64_t run_start = start + i;
        uint32_t run = 0;
        while (i + run < count && run < bmax && !hash_find(start + i + run))
            run++;
        spin_unlock_irq(&cache_lock, flags);

        if (run == 0) { i++; continue; }

        /* Bulk fetch outside the cache lock */
        if (blk_read_bulk(run_start, run, bulk_buf) < 0) {
            /* Fallback: per-block on bulk failure */
            for (uint32_t k = 0; k < run; k++) {
                struct bcache_entry *e = bcache_get(run_start + k);
                if (e) bcache_put(e);
            }
            i += run;
            continue;
        }

        /* Dispatch into cache: for each block, evict if needed and copy. */
        for (uint32_t k = 0; k < run; k++) {
            uint64_t blk = run_start + k;
            spin_lock_irq(&cache_lock, &flags);
            struct bcache_entry *e = hash_find(blk);
            if (e) {
                /* Raced — somebody else loaded it. Skip. */
                spin_unlock_irq(&cache_lock, flags);
                continue;
            }
            e = evict_one();
            if (!e) { spin_unlock_irq(&cache_lock, flags); break; }
            e->block_nr = blk;
            e->dirty = 0;
            e->refcount = 0;     /* not pinned — pure prefetch */
            hash_insert(e);
            /* Insert near tail: don't poke MRU for speculative entries.
             * lru_push_front anyway — they were just loaded; if nobody touches
             * them they fall off naturally. Keeping it simple. */
            lru_push_front(e);
            kmemcpy(e->data, bulk_buf + k * 4096, 4096);
            spin_unlock_irq(&cache_lock, flags);
        }
        i += run;
    }
}
