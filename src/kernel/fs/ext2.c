/* CosmoRT ext2 — read-write ext2 filesystem driver
 *
 * Minimal correct ext2 implementation. All disk access through bcache.
 * Supports: direct blocks, single indirect, double indirect.
 * Skips: triple indirect, xattr, journal.
 */

#include "fs/ext2.h"
#include "fs/bcache.h"
#include "memops.h"
#include "hw/serial.h"
#include "core/mutex.h"
#include "sys/syscall.h"  /* error codes */
#include "core/timer.h"

/* ── State ────────────────────────────────────────── */

static struct ext2_super sb;
static uint32_t block_size;
static uint32_t inodes_per_block;
static uint32_t addrs_per_block;       /* block_size / 4 — pointers per indirect block */
static uint32_t group_count;
static uint32_t inode_size;            /* on-disk inode size (128 or 256) */
static uint32_t desc_per_block;        /* group descriptors per block */
static uint32_t sb_block;              /* block containing superblock */
static int mounted;
static mutex_t fs_lock = MUTEX_INIT;

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

/* ── Block I/O ────────────────────────────────────── */

/* ext2 block numbers map directly to bcache block numbers when block_size == 4096.
 * For 1024-byte blocks, each bcache block (4KB) holds 4 ext2 blocks.
 * For simplicity, we only support block_size == 1024 or 4096. */

static struct bcache_entry *ext2_get_block(uint32_t block) {
    if (block_size == 4096) {
        return bcache_get(block);
    }
    /* 1024-byte blocks: 4 ext2 blocks per bcache block */
    return bcache_get(block / (4096 / block_size));
}

static int ext2_block_offset(uint32_t block) {
    if (block_size == 4096) return 0;
    return (int)((block % (4096 / block_size)) * block_size);
}

/* Write ext2 block from a local buffer */
static int write_block(uint32_t block, const void *buf) {
    struct bcache_entry *be = ext2_get_block(block);
    if (!be) return -EIO;
    kmemcpy(be->data + ext2_block_offset(block), buf, block_size);
    bcache_mark_dirty(be);
    bcache_put(be);
    return 0;
}

/* ── Group descriptor access ─────────────────────── */

static int read_group_desc(uint32_t group, struct ext2_group_desc *gd) {
    /* Group descriptors start at the block after the superblock block.
     * For block_size >= 2048, superblock is in block 0.
     * For block_size == 1024, superblock is in block 1, descriptors in block 2+. */
    uint32_t gd_block = sb.s_first_data_block + 1 + (group * sizeof(struct ext2_group_desc)) / block_size;
    uint32_t gd_offset = (group * sizeof(struct ext2_group_desc)) % block_size;

    struct bcache_entry *be = ext2_get_block(gd_block);
    if (!be) return -EIO;
    kmemcpy(gd, be->data + ext2_block_offset(gd_block) + gd_offset, sizeof(*gd));
    bcache_put(be);
    return 0;
}

static int write_group_desc(uint32_t group, const struct ext2_group_desc *gd) {
    uint32_t gd_block = sb.s_first_data_block + 1 + (group * sizeof(struct ext2_group_desc)) / block_size;
    uint32_t gd_offset = (group * sizeof(struct ext2_group_desc)) % block_size;

    struct bcache_entry *be = ext2_get_block(gd_block);
    if (!be) return -EIO;
    kmemcpy(be->data + ext2_block_offset(gd_block) + gd_offset, gd, sizeof(*gd));
    bcache_mark_dirty(be);
    bcache_put(be);
    return 0;
}

/* ── Superblock persistence ──────────────────────── */

static void write_superblock(void) {
    /* Superblock is always at byte offset 1024 */
    struct bcache_entry *be = bcache_get(sb_block);
    if (!be) return;
    int off = (block_size == 1024) ? 0 : 1024;
    if (block_size == 1024) {
        /* For 1024-byte blocks, sb is in bcache block 0, at offset 1024 within that 4KB page */
        off = 1024;
    }
    kmemcpy(be->data + off, &sb, sizeof(sb));
    bcache_mark_dirty(be);
    bcache_put(be);
}

/* ── Mount ────────────────────────────────────────── */

int ext2_mount(void) {
    /* Superblock is always at byte offset 1024 on disk.
     * bcache uses 4KB pages, so block 0 in bcache covers bytes 0..4095. */
    struct bcache_entry *be = bcache_get(0);
    if (!be) {
        serial_puts("ext2: can't read superblock\n");
        return -EIO;
    }

    kmemcpy(&sb, be->data + 1024, sizeof(sb));
    bcache_put(be);

    if (sb.s_magic != EXT2_MAGIC) {
        serial_puts("ext2: bad magic 0x");
        serial_put_u32(sb.s_magic);
        serial_puts("\n");
        return -EINVAL;
    }

    block_size = 1024U << sb.s_log_block_size;
    if (block_size != 1024 && block_size != 2048 && block_size != 4096) {
        serial_puts("ext2: unsupported block size\n");
        return -EINVAL;
    }

    inode_size = (sb.s_rev_level >= 1) ? sb.s_inode_size : 128;
    inodes_per_block = block_size / inode_size;
    addrs_per_block = block_size / 4;
    desc_per_block = block_size / sizeof(struct ext2_group_desc);
    group_count = (sb.s_blocks_count + sb.s_blocks_per_group - 1) / sb.s_blocks_per_group;

    /* sb_block: the bcache block that contains the superblock (byte 1024) */
    sb_block = (block_size == 1024) ? 0 : 0; /* always bcache block 0 for 4KB pages */

    mounted = 1;

    serial_puts("ext2: mounted (");
    serial_put_u32(sb.s_blocks_count * (block_size / 1024) / 1024);
    serial_puts(" MB, ");
    serial_put_u32(sb.s_free_blocks_count);
    serial_puts(" free blocks, ");
    serial_put_u32(sb.s_free_inodes_count);
    serial_puts(" free inodes)\n");

    return 0;
}

int ext2_unmount(void) {
    if (!mounted) return 0;
    write_superblock();
    bcache_sync();
    mounted = 0;
    serial_puts("ext2: unmounted\n");
    return 0;
}

int ext2_mounted(void) { return mounted; }

uint32_t ext2_block_size(void) { return block_size; }

/* ── Inode read/write ─────────────────────────────── */

int ext2_inode_read(uint32_t ino, struct ext2_inode *out) {
    if (!mounted || ino == 0) return -EINVAL;

    uint32_t group = (ino - 1) / sb.s_inodes_per_group;
    uint32_t index = (ino - 1) % sb.s_inodes_per_group;

    struct ext2_group_desc gd;
    int rc = read_group_desc(group, &gd);
    if (rc < 0) return rc;

    uint32_t inode_block = gd.bg_inode_table + (index * inode_size) / block_size;
    uint32_t inode_off = (index * inode_size) % block_size;

    struct bcache_entry *be = ext2_get_block(inode_block);
    if (!be) return -EIO;
    kmemcpy(out, be->data + ext2_block_offset(inode_block) + inode_off, sizeof(*out));
    bcache_put(be);
    return 0;
}

int ext2_inode_write(uint32_t ino, const struct ext2_inode *in) {
    if (!mounted || ino == 0) return -EINVAL;

    uint32_t group = (ino - 1) / sb.s_inodes_per_group;
    uint32_t index = (ino - 1) % sb.s_inodes_per_group;

    struct ext2_group_desc gd;
    int rc = read_group_desc(group, &gd);
    if (rc < 0) return rc;

    uint32_t inode_block = gd.bg_inode_table + (index * inode_size) / block_size;
    uint32_t inode_off = (index * inode_size) % block_size;

    struct bcache_entry *be = ext2_get_block(inode_block);
    if (!be) return -EIO;
    kmemcpy(be->data + ext2_block_offset(inode_block) + inode_off, in, sizeof(*in));
    bcache_mark_dirty(be);
    bcache_put(be);
    return 0;
}

/* ── Block resolution (logical → physical) ────────── */

/* Read a uint32_t pointer from an indirect block */
static uint32_t read_indirect_ptr(uint32_t ind_block, uint32_t index) {
    struct bcache_entry *be = ext2_get_block(ind_block);
    if (!be) return 0;
    uint32_t *ptrs = (uint32_t *)(be->data + ext2_block_offset(ind_block));
    uint32_t val = ptrs[index];
    bcache_put(be);
    return val;
}

static void write_indirect_ptr(uint32_t ind_block, uint32_t index, uint32_t val) {
    struct bcache_entry *be = ext2_get_block(ind_block);
    if (!be) return;
    uint32_t *ptrs = (uint32_t *)(be->data + ext2_block_offset(ind_block));
    ptrs[index] = val;
    bcache_mark_dirty(be);
    bcache_put(be);
}

/* Resolve logical block to physical. If alloc=1, allocate missing blocks. */
static uint32_t resolve_block(struct ext2_inode *ip, uint32_t file_block, int alloc) {
    /* Direct blocks */
    if (file_block < EXT2_NDIR_BLOCKS) {
        if (ip->i_block[file_block] == 0 && alloc) {
            ip->i_block[file_block] = ext2_block_alloc();
            if (ip->i_block[file_block])
                ip->i_blocks += block_size / 512;
        }
        return ip->i_block[file_block];
    }

    file_block -= EXT2_NDIR_BLOCKS;

    /* Single indirect */
    if (file_block < addrs_per_block) {
        if (ip->i_block[EXT2_IND_BLOCK] == 0) {
            if (!alloc) return 0;
            ip->i_block[EXT2_IND_BLOCK] = ext2_block_alloc();
            if (!ip->i_block[EXT2_IND_BLOCK]) return 0;
            ip->i_blocks += block_size / 512;
            /* Zero the new indirect block */
            uint8_t zero[4096];
            kmemset(zero, 0, block_size);
            write_block(ip->i_block[EXT2_IND_BLOCK], zero);
        }
        uint32_t ptr = read_indirect_ptr(ip->i_block[EXT2_IND_BLOCK], file_block);
        if (ptr == 0 && alloc) {
            ptr = ext2_block_alloc();
            if (ptr) {
                write_indirect_ptr(ip->i_block[EXT2_IND_BLOCK], file_block, ptr);
                ip->i_blocks += block_size / 512;
            }
        }
        return ptr;
    }

    file_block -= addrs_per_block;

    /* Double indirect */
    if (file_block < (uint32_t)addrs_per_block * addrs_per_block) {
        if (ip->i_block[EXT2_DIND_BLOCK] == 0) {
            if (!alloc) return 0;
            ip->i_block[EXT2_DIND_BLOCK] = ext2_block_alloc();
            if (!ip->i_block[EXT2_DIND_BLOCK]) return 0;
            ip->i_blocks += block_size / 512;
            uint8_t zero[4096];
            kmemset(zero, 0, block_size);
            write_block(ip->i_block[EXT2_DIND_BLOCK], zero);
        }
        uint32_t idx1 = file_block / addrs_per_block;
        uint32_t idx2 = file_block % addrs_per_block;

        uint32_t ind2 = read_indirect_ptr(ip->i_block[EXT2_DIND_BLOCK], idx1);
        if (ind2 == 0) {
            if (!alloc) return 0;
            ind2 = ext2_block_alloc();
            if (!ind2) return 0;
            write_indirect_ptr(ip->i_block[EXT2_DIND_BLOCK], idx1, ind2);
            ip->i_blocks += block_size / 512;
            uint8_t zero[4096];
            kmemset(zero, 0, block_size);
            write_block(ind2, zero);
        }

        uint32_t ptr = read_indirect_ptr(ind2, idx2);
        if (ptr == 0 && alloc) {
            ptr = ext2_block_alloc();
            if (ptr) {
                write_indirect_ptr(ind2, idx2, ptr);
                ip->i_blocks += block_size / 512;
            }
        }
        return ptr;
    }

    return 0; /* triple indirect not implemented */
}

/* ── File I/O ─────────────────────────────────────── */

int ext2_read(uint32_t ino, void *buf, size_t offset, size_t len) {
    struct ext2_inode ip;
    int rc = ext2_inode_read(ino, &ip);
    if (rc < 0) return rc;

    uint64_t file_size = ip.i_size;
    /* For regular files rev >= 1, high 32 bits in i_dir_acl */
    if ((ip.i_mode & EXT2_S_IFMT) == EXT2_S_IFREG && sb.s_rev_level >= 1)
        file_size |= (uint64_t)ip.i_dir_acl << 32;

    if (offset >= file_size) return 0;
    if (offset + len > file_size) len = (size_t)(file_size - offset);

    uint8_t *dst = (uint8_t *)buf;
    size_t remaining = len;
    size_t pos = offset;

    while (remaining > 0) {
        uint32_t file_block = (uint32_t)(pos / block_size);
        uint32_t block_off = (uint32_t)(pos % block_size);
        size_t chunk = block_size - block_off;
        if (chunk > remaining) chunk = remaining;

        uint32_t disk_block = resolve_block(&ip, file_block, 0);
        if (disk_block == 0) {
            /* Sparse/hole — read as zeros */
            kmemset(dst, 0, chunk);
        } else {
            struct bcache_entry *be = ext2_get_block(disk_block);
            if (!be) return -EIO;
            kmemcpy(dst, be->data + ext2_block_offset(disk_block) + block_off, chunk);
            bcache_put(be);
        }

        dst += chunk;
        pos += chunk;
        remaining -= chunk;
    }

    return (int)len;
}

int ext2_write(uint32_t ino, const void *buf, size_t offset, size_t len) {
    struct ext2_inode ip;
    int rc = ext2_inode_read(ino, &ip);
    if (rc < 0) return rc;

    const uint8_t *src = (const uint8_t *)buf;
    size_t remaining = len;
    size_t pos = offset;

    while (remaining > 0) {
        uint32_t file_block = (uint32_t)(pos / block_size);
        uint32_t block_off = (uint32_t)(pos % block_size);
        size_t chunk = block_size - block_off;
        if (chunk > remaining) chunk = remaining;

        uint32_t disk_block = resolve_block(&ip, file_block, 1);
        if (disk_block == 0) return -ENOSPC;

        struct bcache_entry *be = ext2_get_block(disk_block);
        if (!be) return -EIO;
        kmemcpy(be->data + ext2_block_offset(disk_block) + block_off, src, chunk);
        bcache_mark_dirty(be);
        bcache_put(be);

        src += chunk;
        pos += chunk;
        remaining -= chunk;
    }

    /* Update size */
    uint64_t new_end = offset + len;
    uint64_t old_size = ip.i_size;
    if ((ip.i_mode & EXT2_S_IFMT) == EXT2_S_IFREG && sb.s_rev_level >= 1)
        old_size |= (uint64_t)ip.i_dir_acl << 32;

    if (new_end > old_size) {
        ip.i_size = (uint32_t)(new_end & 0xFFFFFFFF);
        if ((ip.i_mode & EXT2_S_IFMT) == EXT2_S_IFREG && sb.s_rev_level >= 1)
            ip.i_dir_acl = (uint32_t)(new_end >> 32);
    }
    ip.i_mtime = now_sec();
    ext2_inode_write(ino, &ip);

    return (int)len;
}

/* ── Truncate ─────────────────────────────────────── */

/* Free all blocks referenced by an indirect block */
static void free_indirect_blocks(uint32_t ind_block) {
    if (!ind_block) return;
    struct bcache_entry *be = ext2_get_block(ind_block);
    if (!be) return;
    uint32_t *ptrs = (uint32_t *)(be->data + ext2_block_offset(ind_block));
    for (uint32_t i = 0; i < addrs_per_block; i++) {
        if (ptrs[i]) ext2_block_free(ptrs[i]);
    }
    bcache_put(be);
    ext2_block_free(ind_block);
}

static void free_double_indirect_blocks(uint32_t dind_block) {
    if (!dind_block) return;
    struct bcache_entry *be = ext2_get_block(dind_block);
    if (!be) return;
    uint32_t *ptrs = (uint32_t *)(be->data + ext2_block_offset(dind_block));
    for (uint32_t i = 0; i < addrs_per_block; i++) {
        if (ptrs[i]) free_indirect_blocks(ptrs[i]);
    }
    bcache_put(be);
    ext2_block_free(dind_block);
}

int ext2_truncate(uint32_t ino, size_t new_size) {
    struct ext2_inode ip;
    int rc = ext2_inode_read(ino, &ip);
    if (rc < 0) return rc;

    uint64_t old_size = ip.i_size;
    if ((ip.i_mode & EXT2_S_IFMT) == EXT2_S_IFREG && sb.s_rev_level >= 1)
        old_size |= (uint64_t)ip.i_dir_acl << 32;

    if ((uint64_t)new_size >= old_size) {
        /* Extending: just update size (sparse) */
        ip.i_size = (uint32_t)(new_size & 0xFFFFFFFF);
        if ((ip.i_mode & EXT2_S_IFMT) == EXT2_S_IFREG && sb.s_rev_level >= 1)
            ip.i_dir_acl = (uint32_t)((uint64_t)new_size >> 32);
        ip.i_mtime = now_sec();
        return ext2_inode_write(ino, &ip);
    }

    uint32_t new_blocks = new_size == 0 ? 0 : (uint32_t)((new_size + block_size - 1) / block_size);

    /* Free direct blocks */
    for (uint32_t i = new_blocks; i < EXT2_NDIR_BLOCKS; i++) {
        if (ip.i_block[i]) {
            ext2_block_free(ip.i_block[i]);
            ip.i_block[i] = 0;
        }
    }

    /* Free single indirect */
    if (new_blocks <= EXT2_NDIR_BLOCKS) {
        if (ip.i_block[EXT2_IND_BLOCK]) {
            free_indirect_blocks(ip.i_block[EXT2_IND_BLOCK]);
            ip.i_block[EXT2_IND_BLOCK] = 0;
        }
    } else if (ip.i_block[EXT2_IND_BLOCK]) {
        /* Partial free within indirect block */
        uint32_t start = new_blocks - EXT2_NDIR_BLOCKS;
        struct bcache_entry *be = ext2_get_block(ip.i_block[EXT2_IND_BLOCK]);
        if (be) {
            uint32_t *ptrs = (uint32_t *)(be->data + ext2_block_offset(ip.i_block[EXT2_IND_BLOCK]));
            for (uint32_t i = start; i < addrs_per_block; i++) {
                if (ptrs[i]) { ext2_block_free(ptrs[i]); ptrs[i] = 0; }
            }
            bcache_mark_dirty(be);
            bcache_put(be);
        }
    }

    /* Free double indirect */
    if (new_blocks <= EXT2_NDIR_BLOCKS + addrs_per_block) {
        if (ip.i_block[EXT2_DIND_BLOCK]) {
            free_double_indirect_blocks(ip.i_block[EXT2_DIND_BLOCK]);
            ip.i_block[EXT2_DIND_BLOCK] = 0;
        }
    }

    /* Recalculate i_blocks (512-byte sectors) */
    ip.i_blocks = 0;
    for (int i = 0; i < EXT2_NDIR_BLOCKS; i++)
        if (ip.i_block[i]) ip.i_blocks += block_size / 512;
    if (ip.i_block[EXT2_IND_BLOCK]) {
        ip.i_blocks += block_size / 512; /* the indirect block itself */
        struct bcache_entry *be = ext2_get_block(ip.i_block[EXT2_IND_BLOCK]);
        if (be) {
            uint32_t *ptrs = (uint32_t *)(be->data + ext2_block_offset(ip.i_block[EXT2_IND_BLOCK]));
            for (uint32_t i = 0; i < addrs_per_block; i++)
                if (ptrs[i]) ip.i_blocks += block_size / 512;
            bcache_put(be);
        }
    }

    ip.i_size = (uint32_t)new_size;
    if ((ip.i_mode & EXT2_S_IFMT) == EXT2_S_IFREG && sb.s_rev_level >= 1)
        ip.i_dir_acl = 0;
    ip.i_mtime = now_sec();

    return ext2_inode_write(ino, &ip);
}

/* ── Block allocation ─────────────────────────────── */

uint32_t ext2_block_alloc(void) {
    if (!mounted) return 0;

    mutex_lock(&fs_lock);

    if (sb.s_free_blocks_count == 0) {
        mutex_unlock(&fs_lock);
        return 0;
    }

    for (uint32_t g = 0; g < group_count; g++) {
        struct ext2_group_desc gd;
        if (read_group_desc(g, &gd) < 0) continue;
        if (gd.bg_free_blocks_count == 0) continue;

        /* Scan block bitmap */
        uint32_t bitmap_block = gd.bg_block_bitmap;
        for (uint32_t boff = 0; boff < sb.s_blocks_per_group; boff += block_size * 8) {
            uint32_t bm_blk = bitmap_block + boff / (block_size * 8);
            struct bcache_entry *be = ext2_get_block(bm_blk);
            if (!be) continue;
            uint8_t *bits = be->data + ext2_block_offset(bm_blk);
            uint32_t max_bit = sb.s_blocks_per_group - boff;
            if (max_bit > block_size * 8) max_bit = block_size * 8;

            for (uint32_t byte = 0; byte < max_bit / 8; byte++) {
                if (bits[byte] == 0xFF) continue;
                for (int bit = 0; bit < 8; bit++) {
                    if (boff + byte * 8 + (uint32_t)bit >= sb.s_blocks_per_group) break;
                    if (!(bits[byte] & (1 << bit))) {
                        /* Found free block */
                        bits[byte] |= (uint8_t)(1 << bit);
                        bcache_mark_dirty(be);
                        bcache_put(be);

                        uint32_t block_nr = g * sb.s_blocks_per_group +
                                            sb.s_first_data_block +
                                            boff + byte * 8 + (uint32_t)bit;

                        /* Update group descriptor */
                        gd.bg_free_blocks_count--;
                        write_group_desc(g, &gd);

                        /* Update superblock */
                        sb.s_free_blocks_count--;

                        mutex_unlock(&fs_lock);

                        /* Zero the allocated block */
                        struct bcache_entry *zbe = ext2_get_block(block_nr);
                        if (zbe) {
                            kmemset(zbe->data + ext2_block_offset(block_nr), 0, block_size);
                            bcache_mark_dirty(zbe);
                            bcache_put(zbe);
                        }

                        return block_nr;
                    }
                }
            }
            bcache_put(be);
        }
    }

    mutex_unlock(&fs_lock);
    return 0;
}

void ext2_block_free(uint32_t block) {
    if (!mounted || block == 0) return;

    uint32_t adj_block = block - sb.s_first_data_block;
    uint32_t group = adj_block / sb.s_blocks_per_group;
    uint32_t index = adj_block % sb.s_blocks_per_group;

    if (group >= group_count) return;

    mutex_lock(&fs_lock);

    struct ext2_group_desc gd;
    if (read_group_desc(group, &gd) < 0) {
        mutex_unlock(&fs_lock);
        return;
    }

    uint32_t bm_blk = gd.bg_block_bitmap + index / (block_size * 8);
    uint32_t byte_off = (index % (block_size * 8)) / 8;
    uint32_t bit_off = index % 8;

    struct bcache_entry *be = ext2_get_block(bm_blk);
    if (be) {
        uint8_t *bits = be->data + ext2_block_offset(bm_blk);
        bits[byte_off] &= ~(uint8_t)(1 << bit_off);
        bcache_mark_dirty(be);
        bcache_put(be);

        gd.bg_free_blocks_count++;
        write_group_desc(group, &gd);
        sb.s_free_blocks_count++;
    }

    mutex_unlock(&fs_lock);
}

/* ── Inode allocation ─────────────────────────────── */

uint32_t ext2_inode_alloc(int is_dir) {
    if (!mounted) return 0;

    mutex_lock(&fs_lock);

    for (uint32_t g = 0; g < group_count; g++) {
        struct ext2_group_desc gd;
        if (read_group_desc(g, &gd) < 0) continue;
        if (gd.bg_free_inodes_count == 0) continue;

        /* Scan inode bitmap */
        uint32_t bitmap_block = gd.bg_inode_bitmap;
        struct bcache_entry *be = ext2_get_block(bitmap_block);
        if (!be) continue;
        uint8_t *bits = be->data + ext2_block_offset(bitmap_block);

        for (uint32_t byte = 0; byte < sb.s_inodes_per_group / 8; byte++) {
            if (bits[byte] == 0xFF) continue;
            for (int bit = 0; bit < 8; bit++) {
                uint32_t idx = byte * 8 + (uint32_t)bit;
                if (idx >= sb.s_inodes_per_group) break;
                if (!(bits[byte] & (1 << bit))) {
                    bits[byte] |= (uint8_t)(1 << bit);
                    bcache_mark_dirty(be);
                    bcache_put(be);

                    uint32_t ino = g * sb.s_inodes_per_group + idx + 1;

                    gd.bg_free_inodes_count--;
                    if (is_dir) gd.bg_used_dirs_count++;
                    write_group_desc(g, &gd);

                    sb.s_free_inodes_count--;

                    mutex_unlock(&fs_lock);

                    /* Zero the inode */
                    struct ext2_inode zi;
                    kmemset(&zi, 0, sizeof(zi));
                    ext2_inode_write(ino, &zi);

                    return ino;
                }
            }
        }
        bcache_put(be);
    }

    mutex_unlock(&fs_lock);
    return 0;
}

void ext2_inode_free(uint32_t ino) {
    if (!mounted || ino == 0) return;

    /* Check if it's a directory (for used_dirs_count) */
    struct ext2_inode ip;
    int is_dir = 0;
    if (ext2_inode_read(ino, &ip) == 0)
        is_dir = ((ip.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR);

    uint32_t group = (ino - 1) / sb.s_inodes_per_group;
    uint32_t index = (ino - 1) % sb.s_inodes_per_group;

    mutex_lock(&fs_lock);

    struct ext2_group_desc gd;
    if (read_group_desc(group, &gd) < 0) {
        mutex_unlock(&fs_lock);
        return;
    }

    uint32_t bm_blk = gd.bg_inode_bitmap;
    struct bcache_entry *be = ext2_get_block(bm_blk);
    if (be) {
        uint8_t *bits = be->data + ext2_block_offset(bm_blk);
        uint32_t byte_off = index / 8;
        uint32_t bit_off = index % 8;
        bits[byte_off] &= ~(uint8_t)(1 << bit_off);
        bcache_mark_dirty(be);
        bcache_put(be);

        gd.bg_free_inodes_count++;
        if (is_dir && gd.bg_used_dirs_count > 0) gd.bg_used_dirs_count--;
        write_group_desc(group, &gd);
        sb.s_free_inodes_count++;
    }

    /* Zero the inode on disk */
    struct ext2_inode zi;
    kmemset(&zi, 0, sizeof(zi));
    zi.i_dtime = now_sec();
    ext2_inode_write(ino, &zi);

    mutex_unlock(&fs_lock);
}

/* ── Directory operations ─────────────────────────── */

int ext2_dir_lookup(uint32_t dir_ino, const char *name, uint32_t *child_ino) {
    struct ext2_inode dip;
    int rc = ext2_inode_read(dir_ino, &dip);
    if (rc < 0) return rc;
    if ((dip.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -ENOTDIR;

    uint32_t dir_size = dip.i_size;
    uint32_t pos = 0;

    while (pos < dir_size) {
        uint32_t file_block = pos / block_size;
        uint32_t disk_block = resolve_block(&dip, file_block, 0);
        if (disk_block == 0) { pos += block_size; continue; }

        struct bcache_entry *be = ext2_get_block(disk_block);
        if (!be) return -EIO;
        uint8_t *data = be->data + ext2_block_offset(disk_block);

        uint32_t off = pos % block_size;
        while (off < block_size && pos + off - (pos % block_size) + (pos % block_size) < dir_size) {
            /* Fix offset calculation */
            uint32_t abs_pos = (file_block * block_size) + off;
            if (abs_pos >= dir_size) break;

            struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)(data + off);
            if (de->rec_len == 0) break;  /* corrupt */
            if (de->inode != 0 && de->name_len > 0) {
                if (name_eq(de->name, de->name_len, name)) {
                    *child_ino = de->inode;
                    bcache_put(be);
                    return 0;
                }
            }
            off += de->rec_len;
        }
        bcache_put(be);
        pos = (file_block + 1) * block_size;
    }

    return -ENOENT;
}

int ext2_dir_iterate(uint32_t dir_ino, uint32_t byte_offset,
                     int (*cb)(const char *name, uint32_t ino, uint8_t type,
                               uint32_t next_pos, void *ctx),
                     void *ctx) {
    struct ext2_inode dip;
    int rc = ext2_inode_read(dir_ino, &dip);
    if (rc < 0) return rc;
    if ((dip.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -ENOTDIR;

    uint32_t dir_size = dip.i_size;
    uint32_t cur = byte_offset;           /* resume from caller's position */

    while (cur < dir_size) {
        uint32_t file_block = cur / block_size;
        uint32_t disk_block = resolve_block(&dip, file_block, 0);
        if (disk_block == 0) { cur = (file_block + 1) * block_size; continue; }

        struct bcache_entry *be = ext2_get_block(disk_block);
        if (!be) return -EIO;
        uint8_t *data = be->data + ext2_block_offset(disk_block);

        uint32_t off = cur % block_size;
        while (off < block_size) {
            uint32_t abs_pos = file_block * block_size + off;
            if (abs_pos >= dir_size) break;

            struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)(data + off);
            if (de->rec_len == 0) break;

            uint32_t next_pos = abs_pos + de->rec_len;

            if (de->inode != 0 && de->name_len > 0) {
                /* Build null-terminated name */
                char nbuf[256];
                int nlen = de->name_len;
                if (nlen > 255) nlen = 255;
                kmemcpy(nbuf, de->name, (size_t)nlen);
                nbuf[nlen] = 0;

                if (cb(nbuf, de->inode, de->file_type, next_pos, ctx)) {
                    bcache_put(be);
                    return (int)abs_pos;
                }
            }
            off += de->rec_len;
        }
        bcache_put(be);
        cur = (file_block + 1) * block_size;
    }

    return (int)dir_size;
}

/* Add a directory entry */
int ext2_dir_add(uint32_t dir_ino, const char *name, uint32_t child_ino, uint8_t file_type) {
    struct ext2_inode dip;
    int rc = ext2_inode_read(dir_ino, &dip);
    if (rc < 0) return rc;
    if ((dip.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -ENOTDIR;

    int name_len = kstrlen_s(name);
    /* Required size for new entry: 8 bytes header + name, rounded up to 4 */
    uint16_t needed = (uint16_t)((8 + name_len + 3) & ~3);

    uint32_t dir_size = dip.i_size;
    uint32_t pos = 0;

    /* Scan existing blocks for space */
    while (pos < dir_size) {
        uint32_t file_block = pos / block_size;
        uint32_t disk_block = resolve_block(&dip, file_block, 0);
        if (disk_block == 0) { pos += block_size; continue; }

        struct bcache_entry *be = ext2_get_block(disk_block);
        if (!be) return -EIO;
        uint8_t *data = be->data + ext2_block_offset(disk_block);

        uint32_t off = 0;
        while (off < block_size) {
            struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)(data + off);
            if (de->rec_len == 0) break;

            uint16_t real_len = (uint16_t)((8 + de->name_len + 3) & ~3);
            if (de->inode == 0 && de->rec_len >= needed) {
                /* Reuse deleted entry */
                de->inode = child_ino;
                de->name_len = (uint8_t)name_len;
                de->file_type = file_type;
                kmemcpy(de->name, name, (size_t)name_len);
                bcache_mark_dirty(be);
                bcache_put(be);
                dip.i_mtime = now_sec();
                ext2_inode_write(dir_ino, &dip);
                return 0;
            }
            if (de->inode != 0 && de->rec_len - real_len >= needed) {
                /* Split this entry */
                uint16_t old_rec = de->rec_len;
                de->rec_len = real_len;

                struct ext2_dir_entry_2 *ne = (struct ext2_dir_entry_2 *)(data + off + real_len);
                ne->inode = child_ino;
                ne->rec_len = old_rec - real_len;
                ne->name_len = (uint8_t)name_len;
                ne->file_type = file_type;
                kmemcpy(ne->name, name, (size_t)name_len);

                bcache_mark_dirty(be);
                bcache_put(be);
                dip.i_mtime = now_sec();
                ext2_inode_write(dir_ino, &dip);
                return 0;
            }
            off += de->rec_len;
        }
        bcache_put(be);
        pos = (file_block + 1) * block_size;
    }

    /* No space in existing blocks — allocate a new block */
    uint32_t new_file_block = dir_size / block_size;
    uint32_t new_disk_block = resolve_block(&dip, new_file_block, 1);
    if (new_disk_block == 0) return -ENOSPC;

    /* Initialize the new block with a single entry spanning the whole block */
    struct bcache_entry *be = ext2_get_block(new_disk_block);
    if (!be) return -EIO;
    uint8_t *data = be->data + ext2_block_offset(new_disk_block);
    kmemset(data, 0, block_size);

    struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)data;
    de->inode = child_ino;
    de->rec_len = (uint16_t)block_size;
    de->name_len = (uint8_t)name_len;
    de->file_type = file_type;
    kmemcpy(de->name, name, (size_t)name_len);

    bcache_mark_dirty(be);
    bcache_put(be);

    dip.i_size += block_size;
    dip.i_mtime = now_sec();
    ext2_inode_write(dir_ino, &dip);
    return 0;
}

int ext2_dir_remove(uint32_t dir_ino, const char *name) {
    struct ext2_inode dip;
    int rc = ext2_inode_read(dir_ino, &dip);
    if (rc < 0) return rc;
    if ((dip.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -ENOTDIR;

    uint32_t dir_size = dip.i_size;
    uint32_t pos = 0;

    while (pos < dir_size) {
        uint32_t file_block = pos / block_size;
        uint32_t disk_block = resolve_block(&dip, file_block, 0);
        if (disk_block == 0) { pos += block_size; continue; }

        struct bcache_entry *be = ext2_get_block(disk_block);
        if (!be) return -EIO;
        uint8_t *data = be->data + ext2_block_offset(disk_block);

        uint32_t off = 0;
        struct ext2_dir_entry_2 *prev = 0;
        while (off < block_size) {
            uint32_t abs_pos = file_block * block_size + off;
            if (abs_pos >= dir_size) break;

            struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)(data + off);
            if (de->rec_len == 0) break;

            if (de->inode != 0 && name_eq(de->name, de->name_len, name)) {
                if (prev) {
                    /* Merge with previous entry */
                    prev->rec_len += de->rec_len;
                } else {
                    /* First entry in block — just zero the inode */
                    de->inode = 0;
                }
                bcache_mark_dirty(be);
                bcache_put(be);
                dip.i_mtime = now_sec();
                ext2_inode_write(dir_ino, &dip);
                return 0;
            }
            prev = de;
            off += de->rec_len;
        }
        bcache_put(be);
        pos = (file_block + 1) * block_size;
    }

    return -ENOENT;
}

/* ── Symlink ──────────────────────────────────────── */

int ext2_readlink(uint32_t ino, char *buf, size_t bufsiz) {
    struct ext2_inode ip;
    int rc = ext2_inode_read(ino, &ip);
    if (rc < 0) return rc;
    if ((ip.i_mode & EXT2_S_IFMT) != EXT2_S_IFLNK) return -EINVAL;

    uint32_t len = ip.i_size;
    if (len > bufsiz) len = (uint32_t)bufsiz;

    /* Fast symlink: target stored in i_block if < 60 bytes */
    if (ip.i_blocks == 0 && ip.i_size < 60) {
        kmemcpy(buf, (const char *)ip.i_block, len);
        return (int)len;
    }

    /* Slow symlink: target stored in data blocks */
    return ext2_read(ino, buf, 0, len);
}

/* ── High-level operations ────────────────────────── */

int ext2_create(uint32_t parent_ino, const char *name, uint16_t mode, uint32_t *new_ino) {
    uint32_t ino = ext2_inode_alloc(0);
    if (ino == 0) return -ENOSPC;

    struct ext2_inode ip;
    kmemset(&ip, 0, sizeof(ip));
    ip.i_mode = EXT2_S_IFREG | (mode & 07777);
    ip.i_links_count = 1;
    ip.i_ctime = ip.i_mtime = ip.i_atime = now_sec();
    ext2_inode_write(ino, &ip);

    int rc = ext2_dir_add(parent_ino, name, ino, EXT2_FT_REG_FILE);
    if (rc < 0) {
        ext2_inode_free(ino);
        return rc;
    }

    if (new_ino) *new_ino = ino;
    return 0;
}

int ext2_mkdir(uint32_t parent_ino, const char *name, uint16_t mode, uint32_t *new_ino) {
    uint32_t ino = ext2_inode_alloc(1);
    if (ino == 0) return -ENOSPC;

    struct ext2_inode ip;
    kmemset(&ip, 0, sizeof(ip));
    ip.i_mode = EXT2_S_IFDIR | (mode & 07777);
    ip.i_links_count = 2;  /* . and parent's entry */
    ip.i_ctime = ip.i_mtime = ip.i_atime = now_sec();

    /* Allocate a block for . and .. entries */
    uint32_t blk = ext2_block_alloc();
    if (blk == 0) { ext2_inode_free(ino); return -ENOSPC; }

    ip.i_block[0] = blk;
    ip.i_size = block_size;
    ip.i_blocks = block_size / 512;

    /* Write . and .. entries */
    struct bcache_entry *be = ext2_get_block(blk);
    if (!be) { ext2_block_free(blk); ext2_inode_free(ino); return -EIO; }
    uint8_t *data = be->data + ext2_block_offset(blk);
    kmemset(data, 0, block_size);

    struct ext2_dir_entry_2 *dot = (struct ext2_dir_entry_2 *)data;
    dot->inode = ino;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = EXT2_FT_DIR;
    dot->name[0] = '.';

    struct ext2_dir_entry_2 *dotdot = (struct ext2_dir_entry_2 *)(data + 12);
    dotdot->inode = parent_ino;
    dotdot->rec_len = (uint16_t)(block_size - 12);
    dotdot->name_len = 2;
    dotdot->file_type = EXT2_FT_DIR;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';

    bcache_mark_dirty(be);
    bcache_put(be);

    ext2_inode_write(ino, &ip);

    /* Add to parent directory */
    int rc = ext2_dir_add(parent_ino, name, ino, EXT2_FT_DIR);
    if (rc < 0) {
        ext2_block_free(blk);
        ext2_inode_free(ino);
        return rc;
    }

    /* Increment parent's link count (for ..) */
    struct ext2_inode pip;
    if (ext2_inode_read(parent_ino, &pip) == 0) {
        pip.i_links_count++;
        ext2_inode_write(parent_ino, &pip);
    }

    if (new_ino) *new_ino = ino;
    return 0;
}

int ext2_symlink_create(uint32_t parent_ino, const char *name, const char *target) {
    int tlen = kstrlen_s(target);
    if (tlen == 0 || tlen >= 256) return -ENAMETOOLONG;

    uint32_t ino = ext2_inode_alloc(0);
    if (ino == 0) return -ENOSPC;

    struct ext2_inode ip;
    kmemset(&ip, 0, sizeof(ip));
    ip.i_mode = EXT2_S_IFLNK | 0777;
    ip.i_links_count = 1;
    ip.i_ctime = ip.i_mtime = ip.i_atime = now_sec();
    ip.i_size = (uint32_t)tlen;

    /* Fast symlink: store in i_block if fits (< 60 bytes) */
    if (tlen < 60) {
        kmemcpy((char *)ip.i_block, target, (size_t)tlen);
        ext2_inode_write(ino, &ip);
    } else {
        ext2_inode_write(ino, &ip);
        int rc = ext2_write(ino, target, 0, (size_t)tlen);
        if (rc < 0) {
            ext2_inode_free(ino);
            return rc;
        }
    }

    int rc = ext2_dir_add(parent_ino, name, ino, EXT2_FT_SYMLINK);
    if (rc < 0) {
        ext2_truncate(ino, 0);
        ext2_inode_free(ino);
        return rc;
    }

    return 0;
}

/* ── Rename ───────────────────────────────────────── */

int ext2_rename(uint32_t old_parent, const char *old_name,
                uint32_t new_parent, const char *new_name) {
    uint32_t child_ino;
    int rc = ext2_dir_lookup(old_parent, old_name, &child_ino);
    if (rc < 0) return rc;

    /* Determine file type for new entry */
    struct ext2_inode ip;
    rc = ext2_inode_read(child_ino, &ip);
    if (rc < 0) return rc;
    uint8_t ft = EXT2_FT_REG_FILE;
    uint16_t ftype = ip.i_mode & EXT2_S_IFMT;
    if (ftype == EXT2_S_IFDIR) ft = EXT2_FT_DIR;
    else if (ftype == EXT2_S_IFLNK) ft = EXT2_FT_SYMLINK;

    /* Remove existing target if any */
    uint32_t existing;
    if (ext2_dir_lookup(new_parent, new_name, &existing) == 0) {
        ext2_dir_remove(new_parent, new_name);
        /* Don't free existing inode — link count may still be > 0 */
        struct ext2_inode eip;
        if (ext2_inode_read(existing, &eip) == 0) {
            if (eip.i_links_count > 0) eip.i_links_count--;
            if (eip.i_links_count == 0) {
                ext2_inode_write(existing, &eip);
                ext2_truncate(existing, 0);
                ext2_inode_free(existing);
            } else {
                ext2_inode_write(existing, &eip);
            }
        }
    }

    /* Remove from old parent */
    rc = ext2_dir_remove(old_parent, old_name);
    if (rc < 0) return rc;

    /* Add to new parent */
    rc = ext2_dir_add(new_parent, new_name, child_ino, ft);
    if (rc < 0) {
        /* Try to restore — best effort */
        ext2_dir_add(old_parent, old_name, child_ino, ft);
        return rc;
    }

    /* If moving a directory, update .. to point to new parent */
    if (ft == EXT2_FT_DIR && old_parent != new_parent) {
        struct ext2_inode dip;
        if (ext2_inode_read(child_ino, &dip) == 0) {
            uint32_t blk = resolve_block(&dip, 0, 0);
            if (blk) {
                struct bcache_entry *be = ext2_get_block(blk);
                if (be) {
                    uint8_t *data = be->data + ext2_block_offset(blk);
                    /* Skip . entry */
                    struct ext2_dir_entry_2 *dot = (struct ext2_dir_entry_2 *)data;
                    struct ext2_dir_entry_2 *dotdot = (struct ext2_dir_entry_2 *)(data + dot->rec_len);
                    dotdot->inode = new_parent;
                    bcache_mark_dirty(be);
                    bcache_put(be);
                }
            }
        }
        /* Update link counts */
        struct ext2_inode old_pip;
        if (ext2_inode_read(old_parent, &old_pip) == 0 && old_pip.i_links_count > 0) {
            old_pip.i_links_count--;
            ext2_inode_write(old_parent, &old_pip);
        }
        struct ext2_inode new_pip;
        if (ext2_inode_read(new_parent, &new_pip) == 0) {
            new_pip.i_links_count++;
            ext2_inode_write(new_parent, &new_pip);
        }
    }

    return 0;
}

/* ── Sync ─────────────────────────────────────────── */

void ext2_sync(void) {
    if (!mounted) return;
    write_superblock();
    bcache_sync();
}
