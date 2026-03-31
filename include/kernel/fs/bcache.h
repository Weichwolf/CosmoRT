/* CosmoRT Block Cache — LRU cache for disk blocks
 *
 * All filesystem code accesses disk through bcache, never blk_read/blk_write directly.
 * Hash table for O(1) lookup, LRU list for eviction, write-back with journal protection.
 */
#ifndef BCACHE_H
#define BCACHE_H

#include <stdint.h>

#define BCACHE_SIZE    256   /* 256 × 4KB = 1MB cache */
#define BCACHE_INVALID ((uint64_t)-1)

struct bcache_entry {
    uint64_t block_nr;
    uint8_t *data;               /* page_alloc'd 4KB */
    int dirty;
    int refcount;                /* pinned while in use */
    struct bcache_entry *hash_next;   /* hash chain */
    struct bcache_entry *lru_prev, *lru_next;
};

void bcache_init(void);

/* Get a block into cache, pin it. Reads from disk if not cached. */
struct bcache_entry *bcache_get(uint64_t block);

/* Unpin a cache entry. */
void bcache_put(struct bcache_entry *e);

/* Mark entry as dirty (will be written back on sync/eviction). */
void bcache_mark_dirty(struct bcache_entry *e);

/* Flush all dirty entries to disk. */
void bcache_sync(void);

/* Write a block through cache (gets, copies, marks dirty, puts). */
int bcache_write_block(uint64_t block, const void *data);

#endif
