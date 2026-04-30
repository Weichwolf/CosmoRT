/* CosmoRT Block Cache — LRU cache for disk blocks
 *
 * All filesystem code accesses disk through bcache, never blk_read/blk_write directly.
 * Hash table for O(1) lookup, LRU list for eviction, write-back with journal protection.
 *
 * Multi-instance: jeder Mount-Backend hat seine eigene struct bcache_inst,
 * mit eigenem Block-Source (callbacks). Default-Instanz wraps virtio-blk.
 * Loop-Mounts erzeugen eine Instanz mit pread/pwrite-Callbacks gegen das
 * unterliegende vfs_file*.
 */
#ifndef BCACHE_H
#define BCACHE_H

#include <stdint.h>
#include "spinlock.h"

#define BCACHE_SIZE      1024  /* 1024 × 4KB = 4MB cache (default inst) */
#define BCACHE_INST_SIZE 256   /* per-loop-instance: 256 × 4KB = 1 MB */
#define BCACHE_INVALID   ((uint64_t)-1)
#define BCACHE_HASH_BUCKETS 1024
#define BCACHE_INST_HASH    256

struct bcache_entry {
    uint64_t block_nr;
    uint8_t *data;               /* page_alloc'd 4KB */
    int dirty;
    int refcount;                /* pinned while in use */
    struct bcache_entry *hash_next;   /* hash chain */
    struct bcache_entry *lru_prev, *lru_next;
};

/* Block source backend — abstract read/write/bulk ops. */
struct bcache_backend {
    /* read one 4KB block. return 0 on success, <0 errno on failure. */
    int (*read)(void *ctx, uint64_t block, void *buf);
    /* write one 4KB block. return 0 on success, <0 errno on failure. */
    int (*write)(void *ctx, uint64_t block, const void *buf);
    /* optional: bulk-read 'count' contiguous blocks. NULL → fall back to per-block read. */
    int (*bulk_read)(void *ctx, uint64_t start_block, uint32_t count, void *buf);
    /* optional: max bulk size in blocks. 0 (or NULL bulk_read) ⇒ 1. */
    uint32_t (*bulk_max)(void *ctx);
    void *ctx;                       /* opaque, passed to all callbacks */
};

/* Per-instance cache state. */
struct bcache_inst {
    int                  size;       /* number of entries */
    int                  hash_buckets;
    struct bcache_entry *entries;    /* heap- or static-allocated, len=size */
    struct bcache_entry **hash;      /* len=hash_buckets */
    struct bcache_entry  lru_head;   /* sentinel */
    struct bcache_entry *lru_tail;
    spinlock_t           lock;
    struct bcache_backend backend;
    int                  inited;
};

/* ── Default instance (virtio-blk) ──────────────── */

void bcache_init(void);
struct bcache_inst *bcache_default(void);

/* Legacy single-instance API — operates on bcache_default(). */
struct bcache_entry *bcache_get(uint64_t block);
void bcache_put(struct bcache_entry *e);
void bcache_mark_dirty(struct bcache_entry *e);
void bcache_sync(void);
int  bcache_write_block(uint64_t block, const void *data);
void bcache_readahead(uint64_t start, uint32_t count);

/* ── Per-instance API ───────────────────────────── */

/* Allocate a new instance with size=BCACHE_INST_SIZE. NULL on OOM. */
struct bcache_inst *bcache_inst_create(struct bcache_backend *bk);
void                bcache_inst_destroy(struct bcache_inst *bc);

struct bcache_entry *bcache_get_inst(struct bcache_inst *bc, uint64_t block);
void bcache_put_inst(struct bcache_inst *bc, struct bcache_entry *e);
void bcache_mark_dirty_inst(struct bcache_inst *bc, struct bcache_entry *e);
void bcache_sync_inst(struct bcache_inst *bc);
int  bcache_write_block_inst(struct bcache_inst *bc, uint64_t block, const void *data);
void bcache_readahead_inst(struct bcache_inst *bc, uint64_t start, uint32_t count);

#endif
