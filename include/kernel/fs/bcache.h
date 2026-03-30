/* CosmoRT Block Cache — LRU cache for disk blocks */
#ifndef BCACHE_H
#define BCACHE_H

#include <stdint.h>

#define BCACHE_SIZE    256
#define BCACHE_INVALID ((uint64_t)-1)

struct bcache_entry {
    uint64_t block_nr;
    uint8_t *data;
    int dirty;
    int refcount;
    struct bcache_entry *hash_next;
    struct bcache_entry *lru_prev, *lru_next;
};

void bcache_init(void);

struct bcache_entry *bcache_get(uint64_t block);

void bcache_put(struct bcache_entry *e);

void bcache_mark_dirty(struct bcache_entry *e);

void bcache_sync(void);

int bcache_write_block(uint64_t block, const void *data);

#endif
