/* CosmoRT ext4 — read-write ext4/ext3/ext2 filesystem driver
 *
 * Multi-instance: jede gemountete FS hat eine struct ext4_fs. Default-
 * Instanz fuer Hauptdisk-Mount lebt als globales static; loop-Mounts
 * allokieren via ext4_fs_create. Jede Instanz haelt ihren eigenen
 * Superblock, group-desc-cache, inode-cache und einen Pointer auf eine
 * bcache_inst (default oder loop-private).
 *
 * Korrektheit: alle Funktionen nehmen explizit fs als ersten Parameter.
 * Legacy-API-Wrapper (ext4_walk, ext4_inode_read etc.) delegieren an
 * ext4_default_fs() — Hauptdisk-Mount-Pfade aendern sich nicht.
 *
 * Supports: direct blocks, single indirect, double indirect.
 * Skips: triple indirect, xattr, journal.
 */

#include "fs/ext4.h"
#include "fs/bcache.h"
#include "memops.h"
#include "hw/serial.h"
#include "spinlock.h"
#include "sys/syscall.h"
#include "core/timer.h"
#include "mm/slab.h"

/* ── Default fs instance ──────────────────────────── */

static struct ext4_fs default_fs;
static int default_fs_initialized;

static void default_fs_late_init(void) {
    if (default_fs_initialized) return;
    default_fs.fs_lock = (spinlock_t)SPINLOCK_INIT;
    default_fs.icache_lock = (spinlock_t)SPINLOCK_INIT;
    default_fs.bcache = bcache_default();
    default_fs_initialized = 1;
}

struct ext4_fs *ext4_default_fs(void) {
    default_fs_late_init();
    return &default_fs;
}

/* ── Slab for fs allocation ───────────────────── */
static slab_t ext4_fs_slab;
static int    ext4_fs_slab_ready;

static void fs_slab_ensure(void) {
    if (__sync_bool_compare_and_swap(&ext4_fs_slab_ready, 0, 1))
        slab_init_dynamic(&ext4_fs_slab, sizeof(struct ext4_fs), 0);
}

struct ext4_fs *ext4_fs_create(struct bcache_inst *bc) {
    fs_slab_ensure();
    struct ext4_fs *fs = (struct ext4_fs *)slab_alloc(&ext4_fs_slab);
    if (!fs) return 0;
    char *p = (char *)fs;
    for (size_t i = 0; i < sizeof(*fs); i++) p[i] = 0;
    fs->fs_lock = (spinlock_t)SPINLOCK_INIT;
    fs->icache_lock = (spinlock_t)SPINLOCK_INIT;
    fs->bcache = bc ? bc : bcache_default();
    return fs;
}

void ext4_fs_destroy(struct ext4_fs *fs) {
    if (!fs || fs == &default_fs) return;
    if (fs->mounted) ext4_unmount_inst(fs);
    slab_free(&ext4_fs_slab, fs);
}

/* ── Helpers ──────────────────────────────────────── */

static uint32_t now_sec(void) {
    extern uint32_t timer_epoch_sec(void);
    return timer_epoch_sec();
}

static int kstrlen_s(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int name_eq(const char *a, int alen, const char *b) {
    int blen = kstrlen_s(b);
    if (alen != blen) return 0;
    for (int i = 0; i < alen; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static void serial_put_u32(uint32_t v) {
    char t[12]; int i = 0;
    do { t[i++] = '0' + (char)(v % 10); v /= 10; } while (v);
    while (i--) serial_putchar(t[i]);
}

/* ── Block I/O via bcache_inst ────────────────────── */

/* ext4 block numbers map directly to bcache block numbers when block_size == 4096.
 * For 1024-byte blocks, each bcache block (4KB) holds 4 ext4 blocks. */

static struct bcache_entry *ext4_get_block(struct ext4_fs *fs, uint32_t block) {
    if (fs->block_size == 4096)
        return bcache_get_inst(fs->bcache, block);
    return bcache_get_inst(fs->bcache, block / (4096 / fs->block_size));
}

static int ext4_block_offset(struct ext4_fs *fs, uint32_t block) {
    if (fs->block_size == 4096) return 0;
    return (int)((block % (4096 / fs->block_size)) * fs->block_size);
}

static int write_block(struct ext4_fs *fs, uint32_t block, const void *buf) {
    struct bcache_entry *be = ext4_get_block(fs, block);
    if (!be) return -EIO;
    kmemcpy(be->data + ext4_block_offset(fs, block), buf, fs->block_size);
    bcache_mark_dirty_inst(fs->bcache, be);
    bcache_put_inst(fs->bcache, be);
    return 0;
}

/* ── Group descriptor access ─────────────────────── */

static inline uint32_t gd_disk_block(struct ext4_fs *fs, uint32_t group) {
    return fs->sb.s_first_data_block + 1 +
           (group * sizeof(struct ext4_group_desc)) / fs->block_size;
}
static inline uint32_t gd_disk_offset(struct ext4_fs *fs, uint32_t group) {
    return (group * sizeof(struct ext4_group_desc)) % fs->block_size;
}

static int read_group_desc(struct ext4_fs *fs, uint32_t group, struct ext4_group_desc *gd) {
    if (fs->gd_cache_ready && group < fs->group_count && group < EXT4_MAX_GROUPS) {
        *gd = fs->gd_cache[group];
        return 0;
    }
    uint32_t blk = gd_disk_block(fs, group);
    uint32_t off = gd_disk_offset(fs, group);
    struct bcache_entry *be = ext4_get_block(fs, blk);
    if (!be) return -EIO;
    kmemcpy(gd, be->data + ext4_block_offset(fs, blk) + off, sizeof(*gd));
    bcache_put_inst(fs->bcache, be);
    return 0;
}

static int write_group_desc(struct ext4_fs *fs, uint32_t group, const struct ext4_group_desc *gd) {
    if (fs->gd_cache_ready && group < fs->group_count && group < EXT4_MAX_GROUPS) {
        fs->gd_cache[group] = *gd;
        fs->gd_dirty[group] = 1;
        return 0;
    }
    uint32_t blk = gd_disk_block(fs, group);
    uint32_t off = gd_disk_offset(fs, group);
    struct bcache_entry *be = ext4_get_block(fs, blk);
    if (!be) return -EIO;
    kmemcpy(be->data + ext4_block_offset(fs, blk) + off, gd, sizeof(*gd));
    bcache_mark_dirty_inst(fs->bcache, be);
    bcache_put_inst(fs->bcache, be);
    return 0;
}

static void flush_group_descs(struct ext4_fs *fs) {
    if (!fs->gd_cache_ready) return;
    for (uint32_t g = 0; g < fs->group_count; g++) {
        if (!fs->gd_dirty[g]) continue;
        uint32_t blk = gd_disk_block(fs, g);
        uint32_t off = gd_disk_offset(fs, g);
        struct bcache_entry *be = ext4_get_block(fs, blk);
        if (!be) continue;
        kmemcpy(be->data + ext4_block_offset(fs, blk) + off,
                &fs->gd_cache[g], sizeof(fs->gd_cache[g]));
        bcache_mark_dirty_inst(fs->bcache, be);
        bcache_put_inst(fs->bcache, be);
        fs->gd_dirty[g] = 0;
    }
}

/* ── Inode cache helpers ─────────────────────────── */

static inline uint32_t ic_hash_fn(uint32_t ino) {
    return (ino * 2654435761u) & (EXT4_ICACHE_HASH - 1);
}

static void ic_lru_remove(struct ext4_fs *fs, ext4_icache_entry_t *e) {
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    if (fs->ic_lru_tail == e) fs->ic_lru_tail = e->lru_prev;
    if (fs->ic_lru_tail == &fs->ic_lru_head) fs->ic_lru_tail = 0;
    e->lru_prev = e->lru_next = 0;
}

static void ic_lru_push_front(struct ext4_fs *fs, ext4_icache_entry_t *e) {
    e->lru_prev = &fs->ic_lru_head;
    e->lru_next = fs->ic_lru_head.lru_next;
    if (fs->ic_lru_head.lru_next) fs->ic_lru_head.lru_next->lru_prev = e;
    else                          fs->ic_lru_tail = e;
    fs->ic_lru_head.lru_next = e;
}

static void ic_hash_insert(struct ext4_fs *fs, ext4_icache_entry_t *e) {
    uint32_t h = ic_hash_fn(e->ino);
    e->hash_next = fs->icache_hash[h];
    fs->icache_hash[h] = e;
}

static void ic_hash_remove(struct ext4_fs *fs, ext4_icache_entry_t *e) {
    uint32_t h = ic_hash_fn(e->ino);
    ext4_icache_entry_t **pp = &fs->icache_hash[h];
    while (*pp) {
        if (*pp == e) { *pp = e->hash_next; e->hash_next = 0; return; }
        pp = &(*pp)->hash_next;
    }
}

static ext4_icache_entry_t *ic_hash_find(struct ext4_fs *fs, uint32_t ino) {
    ext4_icache_entry_t *e = fs->icache_hash[ic_hash_fn(ino)];
    while (e) {
        if (e->ino == ino && e->valid) return e;
        e = e->hash_next;
    }
    return 0;
}

static void ic_init(struct ext4_fs *fs) {
    if (fs->icache_ready) return;
    fs->ic_lru_head.lru_next = 0;
    fs->ic_lru_head.lru_prev = 0;
    fs->ic_lru_tail = 0;
    for (int i = 0; i < EXT4_ICACHE_SIZE; i++) {
        fs->icache[i].ino = 0;
        fs->icache[i].valid = 0;
        fs->icache[i].hash_next = 0;
        fs->icache[i].lru_prev = fs->icache[i].lru_next = 0;
        ic_lru_push_front(fs, &fs->icache[i]);
    }
    for (int i = 0; i < EXT4_ICACHE_HASH; i++) fs->icache_hash[i] = 0;
    fs->icache_ready = 1;
}

static void ic_invalidate(struct ext4_fs *fs, uint32_t ino) {
    if (!fs->icache_ready) return;
    uint64_t flags;
    spin_lock_irq(&fs->icache_lock, &flags);
    ext4_icache_entry_t *e = ic_hash_find(fs, ino);
    if (e) {
        ic_hash_remove(fs, e);
        e->valid = 0;
        e->ino = 0;
        ic_lru_remove(fs, e);
        e->lru_prev = fs->ic_lru_tail ? fs->ic_lru_tail : &fs->ic_lru_head;
        e->lru_next = 0;
        if (fs->ic_lru_tail) fs->ic_lru_tail->lru_next = e;
        else                 fs->ic_lru_head.lru_next = e;
        fs->ic_lru_tail = e;
    }
    spin_unlock_irq(&fs->icache_lock, flags);
}

/* ── Superblock persistence ──────────────────────── */

static void write_superblock(struct ext4_fs *fs) {
    struct bcache_entry *be = bcache_get_inst(fs->bcache, fs->sb_block);
    if (!be) return;
    int off = (fs->block_size == 1024) ? 1024 : 1024;
    kmemcpy(be->data + off, &fs->sb, sizeof(fs->sb));
    bcache_mark_dirty_inst(fs->bcache, be);
    bcache_put_inst(fs->bcache, be);
}

/* ── Mount ────────────────────────────────────────── */

int ext4_mount_inst(struct ext4_fs *fs) {
    if (!fs) return -EINVAL;
    if (!fs->bcache) fs->bcache = bcache_default();

    struct bcache_entry *be = bcache_get_inst(fs->bcache, 0);
    if (!be) {
        serial_puts("ext4: can't read superblock\n");
        return -EIO;
    }

    kmemcpy(&fs->sb, be->data + 1024, sizeof(fs->sb));
    bcache_put_inst(fs->bcache, be);

    if (fs->sb.s_magic != EXT4_MAGIC) {
        serial_puts("ext4: bad magic 0x");
        serial_put_u32(fs->sb.s_magic);
        serial_puts("\n");
        return -EINVAL;
    }

    fs->block_size = 1024U << fs->sb.s_log_block_size;
    if (fs->block_size != 1024 && fs->block_size != 2048 && fs->block_size != 4096) {
        serial_puts("ext4: unsupported block size\n");
        return -EINVAL;
    }

    fs->inode_size = (fs->sb.s_rev_level >= 1) ? fs->sb.s_inode_size : 128;
    fs->inodes_per_block = fs->block_size / fs->inode_size;
    fs->addrs_per_block = fs->block_size / 4;
    fs->desc_per_block = fs->block_size / sizeof(struct ext4_group_desc);
    fs->group_count = (fs->sb.s_blocks_count + fs->sb.s_blocks_per_group - 1) /
                      fs->sb.s_blocks_per_group;

    fs->sb_block = 0; /* always bcache block 0 for 4KB pages */

    if (fs->group_count > EXT4_MAX_GROUPS) {
        serial_puts("ext4: group count exceeds cache, gd cache disabled\n");
        fs->gd_cache_ready = 0;
    } else {
        for (uint32_t g = 0; g < fs->group_count; g++) {
            uint32_t blk = gd_disk_block(fs, g);
            uint32_t off = gd_disk_offset(fs, g);
            struct bcache_entry *gbe = ext4_get_block(fs, blk);
            if (!gbe) {
                serial_puts("ext4: gd cache load failed\n");
                fs->gd_cache_ready = 0;
                break;
            }
            kmemcpy(&fs->gd_cache[g], gbe->data + ext4_block_offset(fs, blk) + off,
                    sizeof(fs->gd_cache[g]));
            fs->gd_dirty[g] = 0;
            bcache_put_inst(fs->bcache, gbe);
        }
        fs->gd_cache_ready = 1;
    }

    ic_init(fs);
    fs->mounted = 1;

    serial_puts("ext4: mounted (");
    serial_put_u32(fs->sb.s_blocks_count * (fs->block_size / 1024) / 1024);
    serial_puts(" MB, ");
    serial_put_u32(fs->sb.s_free_blocks_count);
    serial_puts(" free blocks, ");
    serial_put_u32(fs->sb.s_free_inodes_count);
    serial_puts(" free inodes)\n");

    return 0;
}

int ext4_unmount_inst(struct ext4_fs *fs) {
    if (!fs || !fs->mounted) return 0;
    flush_group_descs(fs);
    write_superblock(fs);
    bcache_sync_inst(fs->bcache);
    fs->mounted = 0;
    serial_puts("ext4: unmounted\n");
    return 0;
}

int ext4_mounted_inst(struct ext4_fs *fs) { return fs ? fs->mounted : 0; }

uint32_t ext4_block_size_inst(struct ext4_fs *fs) { return fs ? fs->block_size : 0; }

/* ── Inode read/write ─────────────────────────────── */

int ext4_inode_read_inst(struct ext4_fs *fs, uint32_t ino, struct ext4_inode *out) {
    if (!fs || !fs->mounted || ino == 0) return -EINVAL;

    if (fs->icache_ready) {
        uint64_t flags;
        spin_lock_irq(&fs->icache_lock, &flags);
        ext4_icache_entry_t *e = ic_hash_find(fs, ino);
        if (e) {
            *out = e->data;
            ic_lru_remove(fs, e);
            ic_lru_push_front(fs, e);
            spin_unlock_irq(&fs->icache_lock, flags);
            return 0;
        }
        spin_unlock_irq(&fs->icache_lock, flags);
    }

    uint32_t group = (ino - 1) / fs->sb.s_inodes_per_group;
    uint32_t index = (ino - 1) % fs->sb.s_inodes_per_group;

    struct ext4_group_desc gd;
    int rc = read_group_desc(fs, group, &gd);
    if (rc < 0) return rc;

    uint32_t inode_block = gd.bg_inode_table + (index * fs->inode_size) / fs->block_size;
    uint32_t inode_off = (index * fs->inode_size) % fs->block_size;

    struct bcache_entry *be = ext4_get_block(fs, inode_block);
    if (!be) return -EIO;
    kmemcpy(out, be->data + ext4_block_offset(fs, inode_block) + inode_off, sizeof(*out));
    bcache_put_inst(fs->bcache, be);

    if (fs->icache_ready) {
        uint64_t flags;
        spin_lock_irq(&fs->icache_lock, &flags);
        ext4_icache_entry_t *e = ic_hash_find(fs, ino);
        if (!e) {
            e = fs->ic_lru_tail;
            if (e && e != &fs->ic_lru_head) {
                if (e->valid) ic_hash_remove(fs, e);
                e->ino = ino;
                e->valid = 1;
                e->data = *out;
                ic_lru_remove(fs, e);
                ic_lru_push_front(fs, e);
                ic_hash_insert(fs, e);
            }
        }
        spin_unlock_irq(&fs->icache_lock, flags);
    }
    return 0;
}

int ext4_inode_write_inst(struct ext4_fs *fs, uint32_t ino, const struct ext4_inode *in) {
    if (!fs || !fs->mounted || ino == 0) return -EINVAL;

    uint32_t group = (ino - 1) / fs->sb.s_inodes_per_group;
    uint32_t index = (ino - 1) % fs->sb.s_inodes_per_group;

    struct ext4_group_desc gd;
    int rc = read_group_desc(fs, group, &gd);
    if (rc < 0) return rc;

    uint32_t inode_block = gd.bg_inode_table + (index * fs->inode_size) / fs->block_size;
    uint32_t inode_off = (index * fs->inode_size) % fs->block_size;

    struct bcache_entry *be = ext4_get_block(fs, inode_block);
    if (!be) return -EIO;
    kmemcpy(be->data + ext4_block_offset(fs, inode_block) + inode_off, in, sizeof(*in));
    bcache_mark_dirty_inst(fs->bcache, be);
    bcache_put_inst(fs->bcache, be);

    if (fs->icache_ready) {
        uint64_t flags;
        spin_lock_irq(&fs->icache_lock, &flags);
        ext4_icache_entry_t *e = ic_hash_find(fs, ino);
        if (e) {
            e->data = *in;
            ic_lru_remove(fs, e);
            ic_lru_push_front(fs, e);
        }
        spin_unlock_irq(&fs->icache_lock, flags);
    }
    return 0;
}

/* ── Extra inode fields ─────────────────────────── */

int ext4_has_extra_isize_inst(struct ext4_fs *fs) {
    return fs && fs->mounted && fs->inode_size >= 256;
}

int ext4_read_extra_inst(struct ext4_fs *fs, uint32_t ino, struct ext4_inode_extra *out) {
    if (!ext4_has_extra_isize_inst(fs)) return -ENOTSUP;
    if (ino == 0) return -EINVAL;

    uint32_t group = (ino - 1) / fs->sb.s_inodes_per_group;
    uint32_t index = (ino - 1) % fs->sb.s_inodes_per_group;

    struct ext4_group_desc gd;
    int rc = read_group_desc(fs, group, &gd);
    if (rc < 0) return rc;

    uint32_t inode_block = gd.bg_inode_table + (index * fs->inode_size) / fs->block_size;
    uint32_t inode_off = (index * fs->inode_size) % fs->block_size;

    struct bcache_entry *be = ext4_get_block(fs, inode_block);
    if (!be) return -EIO;
    kmemcpy(out, be->data + ext4_block_offset(fs, inode_block) + inode_off + 128,
            sizeof(*out));
    bcache_put_inst(fs->bcache, be);
    return 0;
}

int ext4_write_extra_inst(struct ext4_fs *fs, uint32_t ino, const struct ext4_inode_extra *in) {
    if (!ext4_has_extra_isize_inst(fs)) return -ENOTSUP;
    if (ino == 0) return -EINVAL;

    uint32_t group = (ino - 1) / fs->sb.s_inodes_per_group;
    uint32_t index = (ino - 1) % fs->sb.s_inodes_per_group;

    struct ext4_group_desc gd;
    int rc = read_group_desc(fs, group, &gd);
    if (rc < 0) return rc;

    uint32_t inode_block = gd.bg_inode_table + (index * fs->inode_size) / fs->block_size;
    uint32_t inode_off = (index * fs->inode_size) % fs->block_size;

    struct bcache_entry *be = ext4_get_block(fs, inode_block);
    if (!be) return -EIO;
    kmemcpy(be->data + ext4_block_offset(fs, inode_block) + inode_off + 128,
            in, sizeof(*in));
    bcache_mark_dirty_inst(fs->bcache, be);
    bcache_put_inst(fs->bcache, be);
    return 0;
}

/* ── Block resolution (logical → physical) ────────── */

static uint32_t read_indirect_ptr(struct ext4_fs *fs, uint32_t ind_block, uint32_t index) {
    struct bcache_entry *be = ext4_get_block(fs, ind_block);
    if (!be) return 0;
    uint32_t *ptrs = (uint32_t *)(be->data + ext4_block_offset(fs, ind_block));
    uint32_t val = ptrs[index];
    bcache_put_inst(fs->bcache, be);
    return val;
}

static void write_indirect_ptr(struct ext4_fs *fs, uint32_t ind_block, uint32_t index, uint32_t val) {
    struct bcache_entry *be = ext4_get_block(fs, ind_block);
    if (!be) return;
    uint32_t *ptrs = (uint32_t *)(be->data + ext4_block_offset(fs, ind_block));
    ptrs[index] = val;
    bcache_mark_dirty_inst(fs->bcache, be);
    bcache_put_inst(fs->bcache, be);
}

static uint32_t resolve_block(struct ext4_fs *fs, struct ext4_inode *ip,
                              uint32_t file_block, int alloc) {
    if (file_block < EXT4_NDIR_BLOCKS) {
        if (ip->i_block[file_block] == 0 && alloc) {
            ip->i_block[file_block] = ext4_block_alloc_inst(fs);
            if (ip->i_block[file_block])
                ip->i_blocks += fs->block_size / 512;
        }
        return ip->i_block[file_block];
    }

    file_block -= EXT4_NDIR_BLOCKS;

    if (file_block < fs->addrs_per_block) {
        if (ip->i_block[EXT4_IND_BLOCK] == 0) {
            if (!alloc) return 0;
            ip->i_block[EXT4_IND_BLOCK] = ext4_block_alloc_inst(fs);
            if (!ip->i_block[EXT4_IND_BLOCK]) return 0;
            ip->i_blocks += fs->block_size / 512;
            uint8_t zero[4096];
            kmemset(zero, 0, fs->block_size);
            write_block(fs, ip->i_block[EXT4_IND_BLOCK], zero);
        }
        uint32_t ptr = read_indirect_ptr(fs, ip->i_block[EXT4_IND_BLOCK], file_block);
        if (ptr == 0 && alloc) {
            ptr = ext4_block_alloc_inst(fs);
            if (ptr) {
                write_indirect_ptr(fs, ip->i_block[EXT4_IND_BLOCK], file_block, ptr);
                ip->i_blocks += fs->block_size / 512;
            }
        }
        return ptr;
    }

    file_block -= fs->addrs_per_block;

    if (file_block < (uint32_t)fs->addrs_per_block * fs->addrs_per_block) {
        if (ip->i_block[EXT4_DIND_BLOCK] == 0) {
            if (!alloc) return 0;
            ip->i_block[EXT4_DIND_BLOCK] = ext4_block_alloc_inst(fs);
            if (!ip->i_block[EXT4_DIND_BLOCK]) return 0;
            ip->i_blocks += fs->block_size / 512;
            uint8_t zero[4096];
            kmemset(zero, 0, fs->block_size);
            write_block(fs, ip->i_block[EXT4_DIND_BLOCK], zero);
        }
        uint32_t idx1 = file_block / fs->addrs_per_block;
        uint32_t idx2 = file_block % fs->addrs_per_block;

        uint32_t ind2 = read_indirect_ptr(fs, ip->i_block[EXT4_DIND_BLOCK], idx1);
        if (ind2 == 0) {
            if (!alloc) return 0;
            ind2 = ext4_block_alloc_inst(fs);
            if (!ind2) return 0;
            write_indirect_ptr(fs, ip->i_block[EXT4_DIND_BLOCK], idx1, ind2);
            ip->i_blocks += fs->block_size / 512;
            uint8_t zero[4096];
            kmemset(zero, 0, fs->block_size);
            write_block(fs, ind2, zero);
        }

        uint32_t ptr = read_indirect_ptr(fs, ind2, idx2);
        if (ptr == 0 && alloc) {
            ptr = ext4_block_alloc_inst(fs);
            if (ptr) {
                write_indirect_ptr(fs, ind2, idx2, ptr);
                ip->i_blocks += fs->block_size / 512;
            }
        }
        return ptr;
    }

    return 0; /* triple indirect not implemented */
}

/* ── File I/O ─────────────────────────────────────── */

#define RA_WINDOW 16

static void file_readahead(struct ext4_fs *fs, struct ext4_inode *ip,
                           uint32_t start_fblk, uint64_t file_size) {
    if (fs->block_size == 0) return;
    uint32_t total_fblk = (uint32_t)((file_size + fs->block_size - 1) / fs->block_size);
    if (start_fblk >= total_fblk) return;

    uint32_t end_fblk = start_fblk + RA_WINDOW;
    if (end_fblk > total_fblk) end_fblk = total_fblk;

    uint64_t run_start = 0;
    uint32_t run = 0;
    for (uint32_t fb = start_fblk; fb < end_fblk; fb++) {
        uint32_t pb = resolve_block(fs, ip, fb, 0);
        if (pb == 0) {
            if (run > 0) bcache_readahead_inst(fs->bcache, run_start, run);
            run = 0;
            continue;
        }
        if (fs->block_size != 4096) {
            if (run > 0) bcache_readahead_inst(fs->bcache, run_start, run);
            run = 0;
            continue;
        }
        if (run == 0) {
            run_start = pb;
            run = 1;
        } else if (pb == run_start + run) {
            run++;
        } else {
            bcache_readahead_inst(fs->bcache, run_start, run);
            run_start = pb;
            run = 1;
        }
    }
    if (run > 0) bcache_readahead_inst(fs->bcache, run_start, run);
}

int ext4_read_inst(struct ext4_fs *fs, uint32_t ino, void *buf, size_t offset, size_t len) {
    if (!fs) return -EINVAL;
    struct ext4_inode ip;
    int rc = ext4_inode_read_inst(fs, ino, &ip);
    if (rc < 0) return rc;

    uint64_t file_size = ip.i_size;
    if ((ip.i_mode & EXT4_S_IFMT) == EXT4_S_IFREG && fs->sb.s_rev_level >= 1)
        file_size |= (uint64_t)ip.i_dir_acl << 32;

    if (offset >= file_size) return 0;
    if (offset + len > file_size) len = (size_t)(file_size - offset);

    if (len >= 8192 && fs->block_size == 4096) {
        uint32_t start_fblk = (uint32_t)(offset / fs->block_size);
        file_readahead(fs, &ip, start_fblk, file_size);
    }

    uint8_t *dst = (uint8_t *)buf;
    size_t remaining = len;
    size_t pos = offset;

    while (remaining > 0) {
        uint32_t file_block = (uint32_t)(pos / fs->block_size);
        uint32_t block_off = (uint32_t)(pos % fs->block_size);
        size_t chunk = fs->block_size - block_off;
        if (chunk > remaining) chunk = remaining;

        uint32_t disk_block = resolve_block(fs, &ip, file_block, 0);
        if (disk_block == 0) {
            kmemset(dst, 0, chunk);
        } else {
            struct bcache_entry *be = ext4_get_block(fs, disk_block);
            if (!be) return -EIO;
            kmemcpy(dst, be->data + ext4_block_offset(fs, disk_block) + block_off, chunk);
            bcache_put_inst(fs->bcache, be);
        }

        dst += chunk;
        pos += chunk;
        remaining -= chunk;
    }

    return (int)len;
}

int ext4_write_inst(struct ext4_fs *fs, uint32_t ino, const void *buf, size_t offset, size_t len) {
    if (!fs) return -EINVAL;
    struct ext4_inode ip;
    int rc = ext4_inode_read_inst(fs, ino, &ip);
    if (rc < 0) return rc;

    const uint8_t *src = (const uint8_t *)buf;
    size_t remaining = len;
    size_t pos = offset;

    while (remaining > 0) {
        uint32_t file_block = (uint32_t)(pos / fs->block_size);
        uint32_t block_off = (uint32_t)(pos % fs->block_size);
        size_t chunk = fs->block_size - block_off;
        if (chunk > remaining) chunk = remaining;

        uint32_t disk_block = resolve_block(fs, &ip, file_block, 1);
        if (disk_block == 0) return -ENOSPC;

        struct bcache_entry *be = ext4_get_block(fs, disk_block);
        if (!be) return -EIO;
        kmemcpy(be->data + ext4_block_offset(fs, disk_block) + block_off, src, chunk);
        bcache_mark_dirty_inst(fs->bcache, be);
        bcache_put_inst(fs->bcache, be);

        src += chunk;
        pos += chunk;
        remaining -= chunk;
    }

    uint64_t new_end = offset + len;
    uint64_t old_size = ip.i_size;
    if ((ip.i_mode & EXT4_S_IFMT) == EXT4_S_IFREG && fs->sb.s_rev_level >= 1)
        old_size |= (uint64_t)ip.i_dir_acl << 32;

    if (new_end > old_size) {
        ip.i_size = (uint32_t)(new_end & 0xFFFFFFFF);
        if ((ip.i_mode & EXT4_S_IFMT) == EXT4_S_IFREG && fs->sb.s_rev_level >= 1)
            ip.i_dir_acl = (uint32_t)(new_end >> 32);
    }
    ip.i_mtime = now_sec();
    ext4_inode_write_inst(fs, ino, &ip);

    return (int)len;
}

/* ── Truncate ─────────────────────────────────────── */

static void free_indirect_blocks(struct ext4_fs *fs, uint32_t ind_block) {
    if (!ind_block) return;
    struct bcache_entry *be = ext4_get_block(fs, ind_block);
    if (!be) return;
    uint32_t *ptrs = (uint32_t *)(be->data + ext4_block_offset(fs, ind_block));
    for (uint32_t i = 0; i < fs->addrs_per_block; i++) {
        if (ptrs[i]) ext4_block_free_inst(fs, ptrs[i]);
    }
    bcache_put_inst(fs->bcache, be);
    ext4_block_free_inst(fs, ind_block);
}

static void free_double_indirect_blocks(struct ext4_fs *fs, uint32_t dind_block) {
    if (!dind_block) return;
    struct bcache_entry *be = ext4_get_block(fs, dind_block);
    if (!be) return;
    uint32_t *ptrs = (uint32_t *)(be->data + ext4_block_offset(fs, dind_block));
    for (uint32_t i = 0; i < fs->addrs_per_block; i++) {
        if (ptrs[i]) free_indirect_blocks(fs, ptrs[i]);
    }
    bcache_put_inst(fs->bcache, be);
    ext4_block_free_inst(fs, dind_block);
}

int ext4_truncate_inst(struct ext4_fs *fs, uint32_t ino, size_t new_size) {
    if (!fs) return -EINVAL;
    struct ext4_inode ip;
    int rc = ext4_inode_read_inst(fs, ino, &ip);
    if (rc < 0) return rc;

    uint64_t old_size = ip.i_size;
    if ((ip.i_mode & EXT4_S_IFMT) == EXT4_S_IFREG && fs->sb.s_rev_level >= 1)
        old_size |= (uint64_t)ip.i_dir_acl << 32;

    if ((uint64_t)new_size >= old_size) {
        ip.i_size = (uint32_t)(new_size & 0xFFFFFFFF);
        if ((ip.i_mode & EXT4_S_IFMT) == EXT4_S_IFREG && fs->sb.s_rev_level >= 1)
            ip.i_dir_acl = (uint32_t)((uint64_t)new_size >> 32);
        ip.i_mtime = now_sec();
        return ext4_inode_write_inst(fs, ino, &ip);
    }

    uint32_t new_blocks = new_size == 0 ? 0 :
                         (uint32_t)((new_size + fs->block_size - 1) / fs->block_size);

    for (uint32_t i = new_blocks; i < EXT4_NDIR_BLOCKS; i++) {
        if (ip.i_block[i]) {
            ext4_block_free_inst(fs, ip.i_block[i]);
            ip.i_block[i] = 0;
        }
    }

    if (new_blocks <= EXT4_NDIR_BLOCKS) {
        if (ip.i_block[EXT4_IND_BLOCK]) {
            free_indirect_blocks(fs, ip.i_block[EXT4_IND_BLOCK]);
            ip.i_block[EXT4_IND_BLOCK] = 0;
        }
    } else if (ip.i_block[EXT4_IND_BLOCK]) {
        uint32_t start = new_blocks - EXT4_NDIR_BLOCKS;
        struct bcache_entry *be = ext4_get_block(fs, ip.i_block[EXT4_IND_BLOCK]);
        if (be) {
            uint32_t *ptrs = (uint32_t *)(be->data + ext4_block_offset(fs, ip.i_block[EXT4_IND_BLOCK]));
            for (uint32_t i = start; i < fs->addrs_per_block; i++) {
                if (ptrs[i]) { ext4_block_free_inst(fs, ptrs[i]); ptrs[i] = 0; }
            }
            bcache_mark_dirty_inst(fs->bcache, be);
            bcache_put_inst(fs->bcache, be);
        }
    }

    if (new_blocks <= EXT4_NDIR_BLOCKS + fs->addrs_per_block) {
        if (ip.i_block[EXT4_DIND_BLOCK]) {
            free_double_indirect_blocks(fs, ip.i_block[EXT4_DIND_BLOCK]);
            ip.i_block[EXT4_DIND_BLOCK] = 0;
        }
    }

    ip.i_blocks = 0;
    for (int i = 0; i < EXT4_NDIR_BLOCKS; i++)
        if (ip.i_block[i]) ip.i_blocks += fs->block_size / 512;
    if (ip.i_block[EXT4_IND_BLOCK]) {
        ip.i_blocks += fs->block_size / 512;
        struct bcache_entry *be = ext4_get_block(fs, ip.i_block[EXT4_IND_BLOCK]);
        if (be) {
            uint32_t *ptrs = (uint32_t *)(be->data + ext4_block_offset(fs, ip.i_block[EXT4_IND_BLOCK]));
            for (uint32_t i = 0; i < fs->addrs_per_block; i++)
                if (ptrs[i]) ip.i_blocks += fs->block_size / 512;
            bcache_put_inst(fs->bcache, be);
        }
    }

    ip.i_size = (uint32_t)new_size;
    if ((ip.i_mode & EXT4_S_IFMT) == EXT4_S_IFREG && fs->sb.s_rev_level >= 1)
        ip.i_dir_acl = 0;
    ip.i_mtime = now_sec();

    return ext4_inode_write_inst(fs, ino, &ip);
}

/* ── Block allocation ─────────────────────────────── */

uint32_t ext4_block_alloc_inst(struct ext4_fs *fs) {
    if (!fs || !fs->mounted) return 0;

    uint64_t flags;
    spin_lock_irq(&fs->fs_lock, &flags);

    if (fs->sb.s_free_blocks_count == 0) {
        spin_unlock_irq(&fs->fs_lock, flags);
        return 0;
    }

    for (uint32_t g = 0; g < fs->group_count; g++) {
        struct ext4_group_desc gd;
        if (read_group_desc(fs, g, &gd) < 0) continue;
        if (gd.bg_free_blocks_count == 0) continue;

        uint32_t bitmap_block = gd.bg_block_bitmap;
        for (uint32_t boff = 0; boff < fs->sb.s_blocks_per_group; boff += fs->block_size * 8) {
            uint32_t bm_blk = bitmap_block + boff / (fs->block_size * 8);
            struct bcache_entry *be = ext4_get_block(fs, bm_blk);
            if (!be) continue;
            uint8_t *bits = be->data + ext4_block_offset(fs, bm_blk);
            uint32_t max_bit = fs->sb.s_blocks_per_group - boff;
            if (max_bit > fs->block_size * 8) max_bit = fs->block_size * 8;

            for (uint32_t byte = 0; byte < max_bit / 8; byte++) {
                if (bits[byte] == 0xFF) continue;
                for (int bit = 0; bit < 8; bit++) {
                    if (boff + byte * 8 + (uint32_t)bit >= fs->sb.s_blocks_per_group) break;
                    if (!(bits[byte] & (1 << bit))) {
                        bits[byte] |= (uint8_t)(1 << bit);
                        bcache_mark_dirty_inst(fs->bcache, be);
                        bcache_put_inst(fs->bcache, be);

                        uint32_t block_nr = g * fs->sb.s_blocks_per_group +
                                            fs->sb.s_first_data_block +
                                            boff + byte * 8 + (uint32_t)bit;

                        gd.bg_free_blocks_count--;
                        write_group_desc(fs, g, &gd);

                        fs->sb.s_free_blocks_count--;

                        spin_unlock_irq(&fs->fs_lock, flags);

                        struct bcache_entry *zbe = ext4_get_block(fs, block_nr);
                        if (zbe) {
                            kmemset(zbe->data + ext4_block_offset(fs, block_nr), 0, fs->block_size);
                            bcache_mark_dirty_inst(fs->bcache, zbe);
                            bcache_put_inst(fs->bcache, zbe);
                        }

                        return block_nr;
                    }
                }
            }
            bcache_put_inst(fs->bcache, be);
        }
    }

    spin_unlock_irq(&fs->fs_lock, flags);
    return 0;
}

void ext4_block_free_inst(struct ext4_fs *fs, uint32_t block) {
    if (!fs || !fs->mounted || block == 0) return;

    uint32_t adj_block = block - fs->sb.s_first_data_block;
    uint32_t group = adj_block / fs->sb.s_blocks_per_group;
    uint32_t index = adj_block % fs->sb.s_blocks_per_group;

    if (group >= fs->group_count) return;

    uint64_t flags;
    spin_lock_irq(&fs->fs_lock, &flags);

    struct ext4_group_desc gd;
    if (read_group_desc(fs, group, &gd) < 0) {
        spin_unlock_irq(&fs->fs_lock, flags);
        return;
    }

    uint32_t bm_blk = gd.bg_block_bitmap + index / (fs->block_size * 8);
    uint32_t byte_off = (index % (fs->block_size * 8)) / 8;
    uint32_t bit_off = index % 8;

    struct bcache_entry *be = ext4_get_block(fs, bm_blk);
    if (be) {
        uint8_t *bits = be->data + ext4_block_offset(fs, bm_blk);
        bits[byte_off] &= ~(uint8_t)(1 << bit_off);
        bcache_mark_dirty_inst(fs->bcache, be);
        bcache_put_inst(fs->bcache, be);

        gd.bg_free_blocks_count++;
        write_group_desc(fs, group, &gd);
        fs->sb.s_free_blocks_count++;
    }

    spin_unlock_irq(&fs->fs_lock, flags);
}

/* ── Inode allocation ─────────────────────────────── */

uint32_t ext4_inode_alloc_inst(struct ext4_fs *fs, int is_dir) {
    if (!fs || !fs->mounted) return 0;

    uint64_t flags;
    spin_lock_irq(&fs->fs_lock, &flags);

    for (uint32_t g = 0; g < fs->group_count; g++) {
        struct ext4_group_desc gd;
        if (read_group_desc(fs, g, &gd) < 0) continue;
        if (gd.bg_free_inodes_count == 0) continue;

        uint32_t bitmap_block = gd.bg_inode_bitmap;
        struct bcache_entry *be = ext4_get_block(fs, bitmap_block);
        if (!be) continue;
        uint8_t *bits = be->data + ext4_block_offset(fs, bitmap_block);

        for (uint32_t byte = 0; byte < fs->sb.s_inodes_per_group / 8; byte++) {
            if (bits[byte] == 0xFF) continue;
            for (int bit = 0; bit < 8; bit++) {
                uint32_t idx = byte * 8 + (uint32_t)bit;
                if (idx >= fs->sb.s_inodes_per_group) break;
                if (!(bits[byte] & (1 << bit))) {
                    bits[byte] |= (uint8_t)(1 << bit);
                    bcache_mark_dirty_inst(fs->bcache, be);
                    bcache_put_inst(fs->bcache, be);

                    uint32_t ino = g * fs->sb.s_inodes_per_group + idx + 1;

                    gd.bg_free_inodes_count--;
                    if (is_dir) gd.bg_used_dirs_count++;
                    write_group_desc(fs, g, &gd);

                    fs->sb.s_free_inodes_count--;

                    spin_unlock_irq(&fs->fs_lock, flags);

                    struct ext4_inode zi;
                    kmemset(&zi, 0, sizeof(zi));
                    ext4_inode_write_inst(fs, ino, &zi);

                    return ino;
                }
            }
        }
        bcache_put_inst(fs->bcache, be);
    }

    spin_unlock_irq(&fs->fs_lock, flags);
    return 0;
}

void ext4_inode_free_inst(struct ext4_fs *fs, uint32_t ino) {
    if (!fs || !fs->mounted || ino == 0) return;

    struct ext4_inode ip;
    int is_dir = 0;
    if (ext4_inode_read_inst(fs, ino, &ip) == 0)
        is_dir = ((ip.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR);

    uint32_t group = (ino - 1) / fs->sb.s_inodes_per_group;
    uint32_t index = (ino - 1) % fs->sb.s_inodes_per_group;

    uint64_t flags;
    spin_lock_irq(&fs->fs_lock, &flags);

    struct ext4_group_desc gd;
    if (read_group_desc(fs, group, &gd) < 0) {
        spin_unlock_irq(&fs->fs_lock, flags);
        return;
    }

    uint32_t bm_blk = gd.bg_inode_bitmap;
    struct bcache_entry *be = ext4_get_block(fs, bm_blk);
    if (be) {
        uint8_t *bits = be->data + ext4_block_offset(fs, bm_blk);
        uint32_t byte_off = index / 8;
        uint32_t bit_off = index % 8;
        bits[byte_off] &= ~(uint8_t)(1 << bit_off);
        bcache_mark_dirty_inst(fs->bcache, be);
        bcache_put_inst(fs->bcache, be);

        gd.bg_free_inodes_count++;
        if (is_dir && gd.bg_used_dirs_count > 0) gd.bg_used_dirs_count--;
        write_group_desc(fs, group, &gd);
        fs->sb.s_free_inodes_count++;
    }

    struct ext4_inode zi;
    kmemset(&zi, 0, sizeof(zi));
    zi.i_dtime = now_sec();
    ext4_inode_write_inst(fs, ino, &zi);
    ic_invalidate(fs, ino);

    spin_unlock_irq(&fs->fs_lock, flags);
}

/* ── Directory operations ─────────────────────────── */

int ext4_dir_lookup_inst(struct ext4_fs *fs, uint32_t dir_ino, const char *name, uint32_t *child_ino) {
    if (!fs) return -EINVAL;
    struct ext4_inode dip;
    int rc = ext4_inode_read_inst(fs, dir_ino, &dip);
    if (rc < 0) return rc;
    if ((dip.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return -ENOTDIR;

    uint32_t dir_size = dip.i_size;
    uint32_t pos = 0;

    while (pos < dir_size) {
        uint32_t file_block = pos / fs->block_size;
        uint32_t disk_block = resolve_block(fs, &dip, file_block, 0);
        if (disk_block == 0) { pos += fs->block_size; continue; }

        struct bcache_entry *be = ext4_get_block(fs, disk_block);
        if (!be) return -EIO;
        uint8_t *data = be->data + ext4_block_offset(fs, disk_block);

        uint32_t off = pos % fs->block_size;
        while (off < fs->block_size) {
            uint32_t abs_pos = (file_block * fs->block_size) + off;
            if (abs_pos >= dir_size) break;

            struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(data + off);
            if (de->rec_len == 0) break;
            if (de->inode != 0 && de->name_len > 0) {
                if (name_eq(de->name, de->name_len, name)) {
                    *child_ino = de->inode;
                    bcache_put_inst(fs->bcache, be);
                    return 0;
                }
            }
            off += de->rec_len;
        }
        bcache_put_inst(fs->bcache, be);
        pos = (file_block + 1) * fs->block_size;
    }

    return -ENOENT;
}

int ext4_dir_iterate_inst(struct ext4_fs *fs, uint32_t dir_ino, uint32_t byte_offset,
                          int (*cb)(const char *name, uint32_t ino, uint8_t type,
                                    uint32_t next_pos, void *ctx),
                          void *ctx) {
    if (!fs) return -EINVAL;
    struct ext4_inode dip;
    int rc = ext4_inode_read_inst(fs, dir_ino, &dip);
    if (rc < 0) return rc;
    if ((dip.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return -ENOTDIR;

    uint32_t dir_size = dip.i_size;
    uint32_t cur = byte_offset;

    while (cur < dir_size) {
        uint32_t file_block = cur / fs->block_size;
        uint32_t disk_block = resolve_block(fs, &dip, file_block, 0);
        if (disk_block == 0) { cur = (file_block + 1) * fs->block_size; continue; }

        struct bcache_entry *be = ext4_get_block(fs, disk_block);
        if (!be) return -EIO;
        uint8_t *data = be->data + ext4_block_offset(fs, disk_block);

        uint32_t off = cur % fs->block_size;
        while (off < fs->block_size) {
            uint32_t abs_pos = file_block * fs->block_size + off;
            if (abs_pos >= dir_size) break;

            struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(data + off);
            if (de->rec_len == 0) break;

            uint32_t next_pos = abs_pos + de->rec_len;

            if (de->inode != 0 && de->name_len > 0) {
                char nbuf[256];
                int nlen = de->name_len;
                if (nlen > 255) nlen = 255;
                kmemcpy(nbuf, de->name, (size_t)nlen);
                nbuf[nlen] = 0;

                if (cb(nbuf, de->inode, de->file_type, next_pos, ctx)) {
                    bcache_put_inst(fs->bcache, be);
                    return (int)abs_pos;
                }
            }
            off += de->rec_len;
        }
        bcache_put_inst(fs->bcache, be);
        cur = (file_block + 1) * fs->block_size;
    }

    return (int)dir_size;
}

int ext4_dir_add_inst(struct ext4_fs *fs, uint32_t dir_ino, const char *name,
                      uint32_t child_ino, uint8_t file_type) {
    if (!fs) return -EINVAL;
    struct ext4_inode dip;
    int rc = ext4_inode_read_inst(fs, dir_ino, &dip);
    if (rc < 0) return rc;
    if ((dip.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return -ENOTDIR;

    int name_len = kstrlen_s(name);
    uint16_t needed = (uint16_t)((8 + name_len + 3) & ~3);

    uint32_t dir_size = dip.i_size;
    uint32_t pos = 0;

    while (pos < dir_size) {
        uint32_t file_block = pos / fs->block_size;
        uint32_t disk_block = resolve_block(fs, &dip, file_block, 0);
        if (disk_block == 0) { pos += fs->block_size; continue; }

        struct bcache_entry *be = ext4_get_block(fs, disk_block);
        if (!be) return -EIO;
        uint8_t *data = be->data + ext4_block_offset(fs, disk_block);

        uint32_t off = 0;
        while (off < fs->block_size) {
            struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(data + off);
            if (de->rec_len == 0) break;

            uint16_t real_len = (uint16_t)((8 + de->name_len + 3) & ~3);
            if (de->inode == 0 && de->rec_len >= needed) {
                de->inode = child_ino;
                de->name_len = (uint8_t)name_len;
                de->file_type = file_type;
                kmemcpy(de->name, name, (size_t)name_len);
                bcache_mark_dirty_inst(fs->bcache, be);
                bcache_put_inst(fs->bcache, be);
                dip.i_mtime = now_sec();
                ext4_inode_write_inst(fs, dir_ino, &dip);
                return 0;
            }
            if (de->inode != 0 && de->rec_len - real_len >= needed) {
                uint16_t old_rec = de->rec_len;
                de->rec_len = real_len;

                struct ext4_dir_entry_2 *ne = (struct ext4_dir_entry_2 *)(data + off + real_len);
                ne->inode = child_ino;
                ne->rec_len = old_rec - real_len;
                ne->name_len = (uint8_t)name_len;
                ne->file_type = file_type;
                kmemcpy(ne->name, name, (size_t)name_len);

                bcache_mark_dirty_inst(fs->bcache, be);
                bcache_put_inst(fs->bcache, be);
                dip.i_mtime = now_sec();
                ext4_inode_write_inst(fs, dir_ino, &dip);
                return 0;
            }
            off += de->rec_len;
        }
        bcache_put_inst(fs->bcache, be);
        pos = (file_block + 1) * fs->block_size;
    }

    uint32_t new_file_block = dir_size / fs->block_size;
    uint32_t new_disk_block = resolve_block(fs, &dip, new_file_block, 1);
    if (new_disk_block == 0) return -ENOSPC;

    struct bcache_entry *be = ext4_get_block(fs, new_disk_block);
    if (!be) return -EIO;
    uint8_t *data = be->data + ext4_block_offset(fs, new_disk_block);
    kmemset(data, 0, fs->block_size);

    struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)data;
    de->inode = child_ino;
    de->rec_len = (uint16_t)fs->block_size;
    de->name_len = (uint8_t)name_len;
    de->file_type = file_type;
    kmemcpy(de->name, name, (size_t)name_len);

    bcache_mark_dirty_inst(fs->bcache, be);
    bcache_put_inst(fs->bcache, be);

    dip.i_size += fs->block_size;
    dip.i_mtime = now_sec();
    ext4_inode_write_inst(fs, dir_ino, &dip);
    return 0;
}

int ext4_dir_remove_inst(struct ext4_fs *fs, uint32_t dir_ino, const char *name) {
    if (!fs) return -EINVAL;
    struct ext4_inode dip;
    int rc = ext4_inode_read_inst(fs, dir_ino, &dip);
    if (rc < 0) return rc;
    if ((dip.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return -ENOTDIR;

    uint32_t dir_size = dip.i_size;
    uint32_t pos = 0;

    while (pos < dir_size) {
        uint32_t file_block = pos / fs->block_size;
        uint32_t disk_block = resolve_block(fs, &dip, file_block, 0);
        if (disk_block == 0) { pos += fs->block_size; continue; }

        struct bcache_entry *be = ext4_get_block(fs, disk_block);
        if (!be) return -EIO;
        uint8_t *data = be->data + ext4_block_offset(fs, disk_block);

        uint32_t off = 0;
        struct ext4_dir_entry_2 *prev = 0;
        while (off < fs->block_size) {
            uint32_t abs_pos = file_block * fs->block_size + off;
            if (abs_pos >= dir_size) break;

            struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(data + off);
            if (de->rec_len == 0) break;

            if (de->inode != 0 && name_eq(de->name, de->name_len, name)) {
                if (prev) {
                    prev->rec_len += de->rec_len;
                } else {
                    de->inode = 0;
                }
                bcache_mark_dirty_inst(fs->bcache, be);
                bcache_put_inst(fs->bcache, be);
                dip.i_mtime = now_sec();
                ext4_inode_write_inst(fs, dir_ino, &dip);
                return 0;
            }
            prev = de;
            off += de->rec_len;
        }
        bcache_put_inst(fs->bcache, be);
        pos = (file_block + 1) * fs->block_size;
    }

    return -ENOENT;
}

/* ── Symlink ──────────────────────────────────────── */

int ext4_readlink_inst(struct ext4_fs *fs, uint32_t ino, char *buf, size_t bufsiz) {
    if (!fs) return -EINVAL;
    struct ext4_inode ip;
    int rc = ext4_inode_read_inst(fs, ino, &ip);
    if (rc < 0) return rc;
    if ((ip.i_mode & EXT4_S_IFMT) != EXT4_S_IFLNK) return -EINVAL;

    uint32_t len = ip.i_size;
    if (len > bufsiz) len = (uint32_t)bufsiz;

    if (ip.i_blocks == 0 && ip.i_size < 60) {
        kmemcpy(buf, (const char *)ip.i_block, len);
        return (int)len;
    }

    return ext4_read_inst(fs, ino, buf, 0, len);
}

/* ── High-level operations ────────────────────────── */

int ext4_create_inst(struct ext4_fs *fs, uint32_t parent_ino, const char *name,
                     uint16_t mode, uint32_t *new_ino) {
    if (!fs) return -EINVAL;
    uint32_t ino = ext4_inode_alloc_inst(fs, 0);
    if (ino == 0) return -ENOSPC;

    struct ext4_inode ip;
    kmemset(&ip, 0, sizeof(ip));
    ip.i_mode = EXT4_S_IFREG | (mode & 07777);
    ip.i_links_count = 1;
    ip.i_ctime = ip.i_mtime = ip.i_atime = now_sec();
    ext4_inode_write_inst(fs, ino, &ip);

    int rc = ext4_dir_add_inst(fs, parent_ino, name, ino, EXT4_FT_REG_FILE);
    if (rc < 0) {
        ext4_inode_free_inst(fs, ino);
        return rc;
    }

    if (new_ino) *new_ino = ino;
    return 0;
}

int ext4_mkdir_inst(struct ext4_fs *fs, uint32_t parent_ino, const char *name,
                    uint16_t mode, uint32_t *new_ino) {
    if (!fs) return -EINVAL;
    uint32_t ino = ext4_inode_alloc_inst(fs, 1);
    if (ino == 0) return -ENOSPC;

    struct ext4_inode ip;
    kmemset(&ip, 0, sizeof(ip));
    ip.i_mode = EXT4_S_IFDIR | (mode & 07777);
    ip.i_links_count = 2;
    ip.i_ctime = ip.i_mtime = ip.i_atime = now_sec();

    uint32_t blk = ext4_block_alloc_inst(fs);
    if (blk == 0) { ext4_inode_free_inst(fs, ino); return -ENOSPC; }

    ip.i_block[0] = blk;
    ip.i_size = fs->block_size;
    ip.i_blocks = fs->block_size / 512;

    struct bcache_entry *be = ext4_get_block(fs, blk);
    if (!be) { ext4_block_free_inst(fs, blk); ext4_inode_free_inst(fs, ino); return -EIO; }
    uint8_t *data = be->data + ext4_block_offset(fs, blk);
    kmemset(data, 0, fs->block_size);

    struct ext4_dir_entry_2 *dot = (struct ext4_dir_entry_2 *)data;
    dot->inode = ino;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = EXT4_FT_DIR;
    dot->name[0] = '.';

    struct ext4_dir_entry_2 *dotdot = (struct ext4_dir_entry_2 *)(data + 12);
    dotdot->inode = parent_ino;
    dotdot->rec_len = (uint16_t)(fs->block_size - 12);
    dotdot->name_len = 2;
    dotdot->file_type = EXT4_FT_DIR;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';

    bcache_mark_dirty_inst(fs->bcache, be);
    bcache_put_inst(fs->bcache, be);

    ext4_inode_write_inst(fs, ino, &ip);

    int rc = ext4_dir_add_inst(fs, parent_ino, name, ino, EXT4_FT_DIR);
    if (rc < 0) {
        ext4_block_free_inst(fs, blk);
        ext4_inode_free_inst(fs, ino);
        return rc;
    }

    struct ext4_inode pip;
    if (ext4_inode_read_inst(fs, parent_ino, &pip) == 0) {
        pip.i_links_count++;
        ext4_inode_write_inst(fs, parent_ino, &pip);
    }

    if (new_ino) *new_ino = ino;
    return 0;
}

int ext4_symlink_create_inst(struct ext4_fs *fs, uint32_t parent_ino,
                             const char *name, const char *target) {
    if (!fs) return -EINVAL;
    int tlen = kstrlen_s(target);
    if (tlen == 0 || tlen >= 256) return -ENAMETOOLONG;

    uint32_t ino = ext4_inode_alloc_inst(fs, 0);
    if (ino == 0) return -ENOSPC;

    struct ext4_inode ip;
    kmemset(&ip, 0, sizeof(ip));
    ip.i_mode = EXT4_S_IFLNK | 0777;
    ip.i_links_count = 1;
    ip.i_ctime = ip.i_mtime = ip.i_atime = now_sec();
    ip.i_size = (uint32_t)tlen;

    if (tlen < 60) {
        kmemcpy((char *)ip.i_block, target, (size_t)tlen);
        ext4_inode_write_inst(fs, ino, &ip);
    } else {
        ext4_inode_write_inst(fs, ino, &ip);
        int rc = ext4_write_inst(fs, ino, target, 0, (size_t)tlen);
        if (rc < 0) {
            ext4_inode_free_inst(fs, ino);
            return rc;
        }
    }

    int rc = ext4_dir_add_inst(fs, parent_ino, name, ino, EXT4_FT_SYMLINK);
    if (rc < 0) {
        ext4_truncate_inst(fs, ino, 0);
        ext4_inode_free_inst(fs, ino);
        return rc;
    }

    return 0;
}

/* ── Rename ───────────────────────────────────────── */

int ext4_rename_inst(struct ext4_fs *fs, uint32_t old_parent, const char *old_name,
                     uint32_t new_parent, const char *new_name) {
    if (!fs) return -EINVAL;
    uint32_t child_ino;
    int rc = ext4_dir_lookup_inst(fs, old_parent, old_name, &child_ino);
    if (rc < 0) return rc;

    struct ext4_inode ip;
    rc = ext4_inode_read_inst(fs, child_ino, &ip);
    if (rc < 0) return rc;
    uint8_t ft = EXT4_FT_REG_FILE;
    uint16_t ftype = ip.i_mode & EXT4_S_IFMT;
    if (ftype == EXT4_S_IFDIR) ft = EXT4_FT_DIR;
    else if (ftype == EXT4_S_IFLNK) ft = EXT4_FT_SYMLINK;

    uint32_t existing;
    if (ext4_dir_lookup_inst(fs, new_parent, new_name, &existing) == 0) {
        ext4_dir_remove_inst(fs, new_parent, new_name);
        struct ext4_inode eip;
        if (ext4_inode_read_inst(fs, existing, &eip) == 0) {
            if (eip.i_links_count > 0) eip.i_links_count--;
            if (eip.i_links_count == 0) {
                ext4_inode_write_inst(fs, existing, &eip);
                ext4_truncate_inst(fs, existing, 0);
                ext4_inode_free_inst(fs, existing);
            } else {
                ext4_inode_write_inst(fs, existing, &eip);
            }
        }
    }

    rc = ext4_dir_remove_inst(fs, old_parent, old_name);
    if (rc < 0) return rc;

    rc = ext4_dir_add_inst(fs, new_parent, new_name, child_ino, ft);
    if (rc < 0) {
        ext4_dir_add_inst(fs, old_parent, old_name, child_ino, ft);
        return rc;
    }

    if (ft == EXT4_FT_DIR && old_parent != new_parent) {
        struct ext4_inode dip;
        if (ext4_inode_read_inst(fs, child_ino, &dip) == 0) {
            uint32_t blk = resolve_block(fs, &dip, 0, 0);
            if (blk) {
                struct bcache_entry *be = ext4_get_block(fs, blk);
                if (be) {
                    uint8_t *data = be->data + ext4_block_offset(fs, blk);
                    struct ext4_dir_entry_2 *dot = (struct ext4_dir_entry_2 *)data;
                    struct ext4_dir_entry_2 *dotdot = (struct ext4_dir_entry_2 *)(data + dot->rec_len);
                    dotdot->inode = new_parent;
                    bcache_mark_dirty_inst(fs->bcache, be);
                    bcache_put_inst(fs->bcache, be);
                }
            }
        }
        struct ext4_inode old_pip;
        if (ext4_inode_read_inst(fs, old_parent, &old_pip) == 0 && old_pip.i_links_count > 0) {
            old_pip.i_links_count--;
            ext4_inode_write_inst(fs, old_parent, &old_pip);
        }
        struct ext4_inode new_pip;
        if (ext4_inode_read_inst(fs, new_parent, &new_pip) == 0) {
            new_pip.i_links_count++;
            ext4_inode_write_inst(fs, new_parent, &new_pip);
        }
    }

    return 0;
}

/* ── Sync ─────────────────────────────────────────── */

void ext4_sync_inst(struct ext4_fs *fs) {
    if (!fs || !fs->mounted) return;
    flush_group_descs(fs);
    write_superblock(fs);
    bcache_sync_inst(fs->bcache);
}

/* ───────────────────────────────────────────────── */
/* ── Default-instance shims (legacy API) ───────── */
/* ───────────────────────────────────────────────── */

int ext4_mount(void)               { return ext4_mount_inst(ext4_default_fs()); }
int ext4_unmount(void)             { return ext4_unmount_inst(ext4_default_fs()); }
int ext4_mounted(void)             { return ext4_mounted_inst(ext4_default_fs()); }
void ext4_sync(void)               { ext4_sync_inst(ext4_default_fs()); }
uint32_t ext4_block_size(void)     { return ext4_block_size_inst(ext4_default_fs()); }

int ext4_inode_read(uint32_t ino, struct ext4_inode *out) {
    return ext4_inode_read_inst(ext4_default_fs(), ino, out);
}
int ext4_inode_write(uint32_t ino, const struct ext4_inode *in) {
    return ext4_inode_write_inst(ext4_default_fs(), ino, in);
}
int ext4_read(uint32_t ino, void *buf, size_t offset, size_t len) {
    return ext4_read_inst(ext4_default_fs(), ino, buf, offset, len);
}
int ext4_write(uint32_t ino, const void *buf, size_t offset, size_t len) {
    return ext4_write_inst(ext4_default_fs(), ino, buf, offset, len);
}
int ext4_truncate(uint32_t ino, size_t new_size) {
    return ext4_truncate_inst(ext4_default_fs(), ino, new_size);
}

int ext4_dir_lookup(uint32_t dir_ino, const char *name, uint32_t *child_ino) {
    return ext4_dir_lookup_inst(ext4_default_fs(), dir_ino, name, child_ino);
}
int ext4_dir_add(uint32_t dir_ino, const char *name, uint32_t child_ino, uint8_t file_type) {
    return ext4_dir_add_inst(ext4_default_fs(), dir_ino, name, child_ino, file_type);
}
int ext4_dir_remove(uint32_t dir_ino, const char *name) {
    return ext4_dir_remove_inst(ext4_default_fs(), dir_ino, name);
}
int ext4_dir_iterate(uint32_t dir_ino, uint32_t byte_offset,
                     int (*cb)(const char *name, uint32_t ino, uint8_t type,
                               uint32_t next_pos, void *ctx),
                     void *ctx) {
    return ext4_dir_iterate_inst(ext4_default_fs(), dir_ino, byte_offset, cb, ctx);
}

uint32_t ext4_inode_alloc(int is_dir)        { return ext4_inode_alloc_inst(ext4_default_fs(), is_dir); }
void     ext4_inode_free(uint32_t ino)       { ext4_inode_free_inst(ext4_default_fs(), ino); }
uint32_t ext4_block_alloc(void)              { return ext4_block_alloc_inst(ext4_default_fs()); }
void     ext4_block_free(uint32_t block)     { ext4_block_free_inst(ext4_default_fs(), block); }

int ext4_create(uint32_t parent_ino, const char *name, uint16_t mode, uint32_t *new_ino) {
    return ext4_create_inst(ext4_default_fs(), parent_ino, name, mode, new_ino);
}
int ext4_mkdir(uint32_t parent_ino, const char *name, uint16_t mode, uint32_t *new_ino) {
    return ext4_mkdir_inst(ext4_default_fs(), parent_ino, name, mode, new_ino);
}
int ext4_symlink_create(uint32_t parent_ino, const char *name, const char *target) {
    return ext4_symlink_create_inst(ext4_default_fs(), parent_ino, name, target);
}
int ext4_readlink(uint32_t ino, char *buf, size_t bufsiz) {
    return ext4_readlink_inst(ext4_default_fs(), ino, buf, bufsiz);
}
int ext4_rename(uint32_t old_parent, const char *old_name,
                uint32_t new_parent, const char *new_name) {
    return ext4_rename_inst(ext4_default_fs(), old_parent, old_name, new_parent, new_name);
}

int ext4_has_extra_isize(void) { return ext4_has_extra_isize_inst(ext4_default_fs()); }
int ext4_read_extra(uint32_t ino, struct ext4_inode_extra *out) {
    return ext4_read_extra_inst(ext4_default_fs(), ino, out);
}
int ext4_write_extra(uint32_t ino, const struct ext4_inode_extra *in) {
    return ext4_write_extra_inst(ext4_default_fs(), ino, in);
}
