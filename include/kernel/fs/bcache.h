/* CosmoRT Block Cache — LRU cache for disk blocks
 *
 * All filesystem code accesses disk through bcache, never blk_read/blk_write directly.
 * Hash table for O(1) lookup, LRU list for eviction, write-back with journal protection.
 */
#ifndef BCACHE_H
#define BCACHE_H

#include <stdint.h>

#define BCACHE_SIZE    1024  /* 1024 × 4KB = 4MB cache */
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

/* Read-ahead: prefetch up to `count` contiguous 4KB blocks starting at `start`.
 * Already-cached ranges are skipped; missing runs are fetched via virtio bulk
 * I/O. Prefetched entries are unpinned (refcount=0); the caller must still
 * bcache_get() the blocks it intends to read, getting cache hits afterwards.
 *
 * Synchronous — returns when all blocks are in cache (or unrecoverable). */
void bcache_readahead(uint64_t start, uint32_t count);

#endif
