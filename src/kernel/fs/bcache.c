/* CosmoRT Block Cache — LRU with hash table lookup */

#include "fs/bcache.h"
#include "mm/page_alloc.h"
#include "memops.h"
#include "hw/serial.h"
#include "spinlock.h"

/* Forward declarations for block driver */
extern int blk_read(uint64_t block, void *buf);
extern int blk_write(uint64_t block, const void *buf);

/* ── Hash table ───────────────────────────────────── */

#define HASH_BUCKETS 64
#define HASH(b) ((b) & (HASH_BUCKETS - 1))

static struct bcache_entry entries[BCACHE_SIZE];
static struct bcache_entry *hash_table[HASH_BUCKETS];

/* LRU list: head = most recently used, tail = least recently used */
static struct bcache_entry lru_head;  /* sentinel */
static spinlock_t cache_lock = SPINLOCK_INIT;

/* ── LRU helpers ──────────────────────────────────── */

static void lru_remove(struct bcache_entry *e) {
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    e->lru_prev = e->lru_next = 0;
}

static void lru_push_front(struct bcache_entry *e) {
    e->lru_next = lru_head.lru_next;
    e->lru_prev = &lru_head;
    if (lru_head.lru_next) lru_head.lru_next->lru_prev = e;
    lru_head.lru_next = e;
}

/* ── Hash helpers ─────────────────────────────────── */

static void hash_insert(struct bcache_entry *e) {
    int h = HASH(e->block_nr);
    e->hash_next = hash_table[h];
    hash_table[h] = e;
}

static void hash_remove(struct bcache_entry *e) {
    int h = HASH(e->block_nr);
    struct bcache_entry **pp = &hash_table[h];
    while (*pp) {
        if (*pp == e) { *pp = e->hash_next; e->hash_next = 0; return; }
        pp = &(*pp)->hash_next;
    }
}

static struct bcache_entry *hash_find(uint64_t block) {
    int h = HASH(block);
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

    for (int i = 0; i < BCACHE_SIZE; i++) {
        entries[i].block_nr = BCACHE_INVALID;
        entries[i].data = (uint8_t *)page_alloc();
        entries[i].dirty = 0;
        entries[i].refcount = 0;
        entries[i].hash_next = 0;
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
    serial_puts(" blocks, 1MB)\n");
}

/* ── Evict LRU entry ─────────────────────────────── */

static struct bcache_entry *evict_one(void) {
    /* Walk LRU from tail (least recently used) */
    struct bcache_entry *e = lru_head.lru_prev;
    /* lru_head.lru_prev is the tail (sentinel has no lru_prev initially).
     * Walk backwards from the end of the list. */

    /* Find tail by walking forward from head */
    struct bcache_entry *tail = lru_head.lru_next;
    if (!tail) return 0;
    while (tail->lru_next) tail = tail->lru_next;

    /* Walk backwards from tail to find unpinned entry */
    e = tail;
    while (e && e != &lru_head) {
        if (e->refcount == 0) {
            /* Flush if dirty */
            if (e->dirty && e->block_nr != BCACHE_INVALID) {
                blk_write(e->block_nr, e->data);
                e->dirty = 0;
            }
            /* Remove from hash and LRU */
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
        /* Read failed — remove from cache */
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
