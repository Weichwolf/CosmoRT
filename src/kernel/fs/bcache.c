/* CosmoRT Block Cache — LRU with hash table lookup
 *
 * Multi-instance: bcache_inst kapselt eine eigene LRU + Hash. Default-
 * Instanz hat 1024 Eintraege (4 MB) und benutzt die virtio-blk Callbacks.
 * Loop-Mounts allokieren eine 256-Eintrag-Instanz (1 MB) mit pread/pwrite-
 * Callbacks gegen das backing-vfs_file*.
 *
 * Synchronous bulk read-ahead via virtio-blk bulk-read API: contiguous
 * block ranges get fetched in a single virtio request (one IRQ, one memcpy
 * out of the DMA buffer).
 */

#include "fs/bcache.h"
#include "mm/page_alloc.h"
#include "mm/slab.h"
#include "memops.h"
#include "hw/serial.h"

/* Block driver (virtio-blk) — default backend ctx. */
extern int blk_read(uint64_t block, void *buf);
extern int blk_write(uint64_t block, const void *buf);
extern int blk_read_bulk(uint64_t start_block, uint32_t count, void *buf);
extern uint32_t blk_bulk_max(void);

/* ── Default backend wrappers ──────────────────── */

static int virtio_read(void *ctx, uint64_t block, void *buf) {
    (void)ctx; return blk_read(block, buf);
}
static int virtio_write(void *ctx, uint64_t block, const void *buf) {
    (void)ctx; return blk_write(block, buf);
}
static int virtio_bulk(void *ctx, uint64_t start, uint32_t count, void *buf) {
    (void)ctx; return blk_read_bulk(start, count, buf);
}
static uint32_t virtio_bulk_max(void *ctx) {
    (void)ctx; return blk_bulk_max();
}

/* ── Default instance state ──────────────────── */

static struct bcache_entry  default_entries[BCACHE_SIZE];
static struct bcache_entry *default_hash[BCACHE_HASH_BUCKETS];
static struct bcache_inst   default_inst;

/* ── Hash + LRU primitives (operate on bc) ────── */

static inline uint32_t bcache_hash_fn(uint64_t b, int buckets) {
    return (uint32_t)(b * 2654435761u) & (uint32_t)(buckets - 1);
}

static void lru_remove(struct bcache_inst *bc, struct bcache_entry *e) {
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    if (bc->lru_tail == e) bc->lru_tail = e->lru_prev;
    if (bc->lru_tail == &bc->lru_head) bc->lru_tail = 0;
    e->lru_prev = e->lru_next = 0;
}

static void lru_push_front(struct bcache_inst *bc, struct bcache_entry *e) {
    e->lru_prev = &bc->lru_head;
    e->lru_next = bc->lru_head.lru_next;
    if (bc->lru_head.lru_next) bc->lru_head.lru_next->lru_prev = e;
    else                       bc->lru_tail = e;
    bc->lru_head.lru_next = e;
}

static void hash_insert(struct bcache_inst *bc, struct bcache_entry *e) {
    uint32_t h = bcache_hash_fn(e->block_nr, bc->hash_buckets);
    e->hash_next = bc->hash[h];
    bc->hash[h] = e;
}

static void hash_remove(struct bcache_inst *bc, struct bcache_entry *e) {
    uint32_t h = bcache_hash_fn(e->block_nr, bc->hash_buckets);
    struct bcache_entry **pp = &bc->hash[h];
    while (*pp) {
        if (*pp == e) { *pp = e->hash_next; e->hash_next = 0; return; }
        pp = &(*pp)->hash_next;
    }
}

static struct bcache_entry *hash_find(struct bcache_inst *bc, uint64_t block) {
    uint32_t h = bcache_hash_fn(block, bc->hash_buckets);
    struct bcache_entry *e = bc->hash[h];
    while (e) {
        if (e->block_nr == block) return e;
        e = e->hash_next;
    }
    return 0;
}

/* ── Init helpers ───────────────────────────── */

static void inst_init_internal(struct bcache_inst *bc) {
    bc->lru_head.lru_next = 0;
    bc->lru_head.lru_prev = 0;
    bc->lru_tail = 0;
    for (int i = 0; i < bc->size; i++) {
        bc->entries[i].block_nr = BCACHE_INVALID;
        bc->entries[i].dirty = 0;
        bc->entries[i].refcount = 0;
        bc->entries[i].hash_next = 0;
        bc->entries[i].lru_prev = bc->entries[i].lru_next = 0;
        if (!bc->entries[i].data) {
            bc->entries[i].data = (uint8_t *)page_alloc();
            if (!bc->entries[i].data) {
                serial_puts("bcache: page_alloc failed\n");
                return;
            }
        }
        lru_push_front(bc, &bc->entries[i]);
    }
    for (int i = 0; i < bc->hash_buckets; i++)
        bc->hash[i] = 0;
    bc->inited = 1;
}

void bcache_init(void) {
    default_inst.size         = BCACHE_SIZE;
    default_inst.hash_buckets = BCACHE_HASH_BUCKETS;
    default_inst.entries      = default_entries;
    default_inst.hash         = default_hash;
    default_inst.lock         = (spinlock_t)SPINLOCK_INIT;
    default_inst.backend.read     = virtio_read;
    default_inst.backend.write    = virtio_write;
    default_inst.backend.bulk_read = virtio_bulk;
    default_inst.backend.bulk_max  = virtio_bulk_max;
    default_inst.backend.ctx      = 0;
    inst_init_internal(&default_inst);

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

struct bcache_inst *bcache_default(void) { return &default_inst; }

/* ── Slab for ad-hoc inst allocation ─────────── */
static slab_t bcache_inst_slab;
static int    bcache_inst_slab_ready;

static void inst_slab_ensure(void) {
    if (__sync_bool_compare_and_swap(&bcache_inst_slab_ready, 0, 1)) {
        slab_init_dynamic(&bcache_inst_slab, sizeof(struct bcache_inst), 0);
    }
}

struct bcache_inst *bcache_inst_create(struct bcache_backend *bk) {
    if (!bk) return 0;
    inst_slab_ensure();
    struct bcache_inst *bc = (struct bcache_inst *)slab_alloc(&bcache_inst_slab);
    if (!bc) return 0;
    /* zero whole thing */
    char *p = (char *)bc;
    for (size_t i = 0; i < sizeof(*bc); i++) p[i] = 0;

    bc->size         = BCACHE_INST_SIZE;
    bc->hash_buckets = BCACHE_INST_HASH;
    bc->lock         = (spinlock_t)SPINLOCK_INIT;
    bc->backend      = *bk;

    /* entries[]: 256 × sizeof(bcache_entry) ≈ 256×56 = 14 KB → pages_alloc.
     * hash[]: 256 × sizeof(ptr) = 2 KB → pages_alloc.  */
    int entries_bytes = bc->size * (int)sizeof(struct bcache_entry);
    int entries_pages = (entries_bytes + 4095) / 4096;
    int p2 = 1; while (p2 < entries_pages) p2 *= 2;
    bc->entries = (struct bcache_entry *)pages_alloc(p2);
    if (!bc->entries) { slab_free(&bcache_inst_slab, bc); return 0; }
    /* Zero entries — needed because inst_init_internal checks data==NULL */
    char *ep = (char *)bc->entries;
    for (int i = 0; i < entries_pages * 4096; i++) ep[i] = 0;

    int hash_bytes = bc->hash_buckets * (int)sizeof(struct bcache_entry *);
    int hash_pages = (hash_bytes + 4095) / 4096;
    int hp2 = 1; while (hp2 < hash_pages) hp2 *= 2;
    bc->hash = (struct bcache_entry **)pages_alloc(hp2);
    if (!bc->hash) {
        pages_free(bc->entries, p2);
        slab_free(&bcache_inst_slab, bc);
        return 0;
    }
    char *hp = (char *)bc->hash;
    for (int i = 0; i < hash_pages * 4096; i++) hp[i] = 0;

    inst_init_internal(bc);
    return bc;
}

void bcache_inst_destroy(struct bcache_inst *bc) {
    if (!bc || bc == &default_inst) return;
    bcache_sync_inst(bc);
    /* free per-entry data pages */
    for (int i = 0; i < bc->size; i++) {
        if (bc->entries[i].data) page_free(bc->entries[i].data);
    }
    int entries_bytes = bc->size * (int)sizeof(struct bcache_entry);
    int entries_pages = (entries_bytes + 4095) / 4096;
    int p2 = 1; while (p2 < entries_pages) p2 *= 2;
    pages_free(bc->entries, p2);

    int hash_bytes = bc->hash_buckets * (int)sizeof(struct bcache_entry *);
    int hash_pages = (hash_bytes + 4095) / 4096;
    int hp2 = 1; while (hp2 < hash_pages) hp2 *= 2;
    pages_free(bc->hash, hp2);

    slab_free(&bcache_inst_slab, bc);
}

/* ── Evict ────────────────────────────────────── */
static struct bcache_entry *evict_one(struct bcache_inst *bc) {
    struct bcache_entry *e = bc->lru_tail;
    while (e && e != &bc->lru_head) {
        if (e->refcount == 0) {
            if (e->dirty && e->block_nr != BCACHE_INVALID) {
                bc->backend.write(bc->backend.ctx, e->block_nr, e->data);
                e->dirty = 0;
            }
            if (e->block_nr != BCACHE_INVALID)
                hash_remove(bc, e);
            lru_remove(bc, e);
            e->block_nr = BCACHE_INVALID;
            return e;
        }
        e = e->lru_prev;
    }
    serial_puts("bcache: all entries pinned!\n");
    return 0;
}

/* ── Get / Put / Mark / Sync (per-inst) ───────── */

struct bcache_entry *bcache_get_inst(struct bcache_inst *bc, uint64_t block) {
    if (!bc || !bc->inited) return 0;
    uint64_t flags;
    spin_lock_irq(&bc->lock, &flags);

    struct bcache_entry *e = hash_find(bc, block);
    if (e) {
        e->refcount++;
        lru_remove(bc, e);
        lru_push_front(bc, e);
        spin_unlock_irq(&bc->lock, flags);
        return e;
    }

    e = evict_one(bc);
    if (!e) {
        spin_unlock_irq(&bc->lock, flags);
        return 0;
    }

    e->block_nr = block;
    e->dirty = 0;
    e->refcount = 1;
    hash_insert(bc, e);
    lru_push_front(bc, e);

    spin_unlock_irq(&bc->lock, flags);

    if (bc->backend.read(bc->backend.ctx, block, e->data) < 0) {
        spin_lock_irq(&bc->lock, &flags);
        hash_remove(bc, e);
        lru_remove(bc, e);
        e->block_nr = BCACHE_INVALID;
        e->refcount = 0;
        lru_push_front(bc, e);
        spin_unlock_irq(&bc->lock, flags);
        return 0;
    }

    return e;
}

void bcache_put_inst(struct bcache_inst *bc, struct bcache_entry *e) {
    if (!e || !bc) return;
    uint64_t flags;
    spin_lock_irq(&bc->lock, &flags);
    if (e->refcount > 0) e->refcount--;
    spin_unlock_irq(&bc->lock, flags);
}

void bcache_mark_dirty_inst(struct bcache_inst *bc, struct bcache_entry *e) {
    (void)bc;
    if (e) e->dirty = 1;
}

void bcache_sync_inst(struct bcache_inst *bc) {
    if (!bc || !bc->inited) return;
    uint64_t flags;
    spin_lock_irq(&bc->lock, &flags);
    for (int i = 0; i < bc->size; i++) {
        if (bc->entries[i].dirty && bc->entries[i].block_nr != BCACHE_INVALID) {
            bc->backend.write(bc->backend.ctx, bc->entries[i].block_nr,
                              bc->entries[i].data);
            bc->entries[i].dirty = 0;
        }
    }
    spin_unlock_irq(&bc->lock, flags);
}

int bcache_write_block_inst(struct bcache_inst *bc, uint64_t block, const void *data) {
    struct bcache_entry *e = bcache_get_inst(bc, block);
    if (!e) return -1;
    kmemcpy(e->data, data, 4096);
    bcache_mark_dirty_inst(bc, e);
    bcache_put_inst(bc, e);
    return 0;
}

/* ── Read-ahead (per-inst) ────────────────────── */

void bcache_readahead_inst(struct bcache_inst *bc, uint64_t start, uint32_t count) {
    if (!bc || count == 0) return;
    uint32_t bmax = bc->backend.bulk_max ? bc->backend.bulk_max(bc->backend.ctx) : 1;
    if (bmax == 0) bmax = 1;

    /* Static landing buffer for bulk DMA → cache copy. 64KB (16 blocks).
     * Read-ahead happens under fs_lock paths; protect against parallel CPUs
     * with the cache_lock window during dispatch. */
    static uint8_t bulk_buf[16 * 4096] __attribute__((aligned(4096)));
    if (bmax > 16) bmax = 16;

    uint32_t i = 0;
    while (i < count) {
        uint64_t flags;
        spin_lock_irq(&bc->lock, &flags);
        while (i < count && hash_find(bc, start + i)) i++;
        if (i >= count) { spin_unlock_irq(&bc->lock, flags); return; }
        uint64_t run_start = start + i;
        uint32_t run = 0;
        while (i + run < count && run < bmax && !hash_find(bc, start + i + run))
            run++;
        spin_unlock_irq(&bc->lock, flags);

        if (run == 0) { i++; continue; }

        int rc;
        if (bc->backend.bulk_read)
            rc = bc->backend.bulk_read(bc->backend.ctx, run_start, run, bulk_buf);
        else
            rc = -1; /* force fallback */

        if (rc < 0) {
            /* Fallback: per-block on bulk failure */
            for (uint32_t k = 0; k < run; k++) {
                struct bcache_entry *e = bcache_get_inst(bc, run_start + k);
                if (e) bcache_put_inst(bc, e);
            }
            i += run;
            continue;
        }

        /* Dispatch into cache */
        for (uint32_t k = 0; k < run; k++) {
            uint64_t blk = run_start + k;
            spin_lock_irq(&bc->lock, &flags);
            struct bcache_entry *e = hash_find(bc, blk);
            if (e) {
                spin_unlock_irq(&bc->lock, flags);
                continue;
            }
            e = evict_one(bc);
            if (!e) { spin_unlock_irq(&bc->lock, flags); break; }
            e->block_nr = blk;
            e->dirty = 0;
            e->refcount = 0;
            hash_insert(bc, e);
            lru_push_front(bc, e);
            kmemcpy(e->data, bulk_buf + k * 4096, 4096);
            spin_unlock_irq(&bc->lock, flags);
        }
        i += run;
    }
}

/* ── Default-instance shims (legacy API) ─────── */

struct bcache_entry *bcache_get(uint64_t block) {
    return bcache_get_inst(&default_inst, block);
}
void bcache_put(struct bcache_entry *e) { bcache_put_inst(&default_inst, e); }
void bcache_mark_dirty(struct bcache_entry *e) { bcache_mark_dirty_inst(&default_inst, e); }
void bcache_sync(void) { bcache_sync_inst(&default_inst); }
int  bcache_write_block(uint64_t block, const void *data) {
    return bcache_write_block_inst(&default_inst, block, data);
}
void bcache_readahead(uint64_t start, uint32_t count) {
    bcache_readahead_inst(&default_inst, start, count);
}
