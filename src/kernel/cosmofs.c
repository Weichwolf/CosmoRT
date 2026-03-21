/* CosmoRT CosmoFS — persistent filesystem core
 *
 * Superblock, inode allocation, block allocation, file data I/O,
 * directory operations (B+ tree), attribute stubs.
 * All disk access through bcache. All metadata mutations through journal.
 */

#include "cosmofs.h"
#include "bcache.h"
#include "btree.h"
#include "journal.h"
#include "memops.h"
#include "serial.h"
#include "spinlock.h"
#include "syscall.h"  /* error codes */
#include "timer.h"

/* ── State ────────────────────────────────────────── */

static struct cosmofs_super sb;
static int mounted;
static spinlock_t fs_lock = SPINLOCK_INIT;

/* ── Helpers ──────────────────────────────────────── */

static uint64_t now_ns(void) {
    return timer_ms() * 1000000ULL;
}

/* ── Mount ────────────────────────────────────────── */

int cosmofs_mount(void) {
    /* Read superblock from block 0 */
    struct bcache_entry *be = bcache_get(0);
    if (!be) {
        serial_puts("cosmofs: can't read superblock\n");
        return -EIO;
    }

    kmemcpy(&sb, be->data, sizeof(sb));
    bcache_put(be);

    if (sb.magic != COSMOFS_MAGIC_U64) {
        serial_puts("cosmofs: bad magic\n");
        return -EINVAL;
    }

    if (sb.version != COSMOFS_VERSION) {
        serial_puts("cosmofs: unsupported version\n");
        return -EINVAL;
    }

    /* Init journal */
    journal_init(sb.journal_start, sb.journal_size);
    journal_recover();

    mounted = 1;

    serial_puts("cosmofs: mounted (");
    { char t[20]; int i=0; uint64_t mb = sb.total_blocks * 4 / 1024;
      do{t[i++]='0'+(char)(mb%10);mb/=10;}while(mb);
      while(i--) serial_putchar(t[i]); }
    serial_puts(" MB, ");
    { char t[20]; int i=0; uint64_t v = sb.free_blocks;
      do{t[i++]='0'+(char)(v%10);v/=10;}while(v);
      while(i--) serial_putchar(t[i]); }
    serial_puts(" free)\n");

    return 0;
}

int cosmofs_unmount(void) {
    if (!mounted) return 0;
    bcache_sync();

    /* Write superblock back */
    struct bcache_entry *be = bcache_get(0);
    if (be) {
        kmemcpy(be->data, &sb, sizeof(sb));
        bcache_mark_dirty(be);
        bcache_put(be);
    }
    bcache_sync();

    mounted = 0;
    serial_puts("cosmofs: unmounted\n");
    return 0;
}

int cosmofs_mounted(void) { return mounted; }

uint64_t cosmofs_root_ino(void) { return sb.root_inode; }

/* ── Block allocation (bitmap) ────────────────────── */

uint64_t cosmofs_block_alloc(void) {
    if (!mounted) return 0;

    uint64_t flags;
    spin_lock_irq(&fs_lock, &flags);

    if (sb.free_blocks == 0) {
        spin_unlock_irq(&fs_lock, flags);
        return 0;
    }

    /* Scan bitmap for free block */
    for (uint32_t bi = 0; bi < sb.bitmap_blocks; bi++) {
        struct bcache_entry *be = bcache_get(sb.bitmap_start + bi);
        if (!be) continue;

        uint64_t *bits = (uint64_t *)be->data;
        for (int w = 0; w < 512; w++) {  /* 512 uint64 per 4KB block */
            if (bits[w] != ~0ULL) {
                /* Find first zero bit */
                uint64_t mask = bits[w];
                int bit = __builtin_ctzll(~mask);
                bits[w] |= (1ULL << bit);
                bcache_mark_dirty(be);

                uint64_t block_nr = sb.data_start +
                    (uint64_t)bi * (4096 * 8) + (uint64_t)w * 64 + (uint64_t)bit;

                if (block_nr >= sb.total_blocks) {
                    /* Beyond disk — undo */
                    bits[w] &= ~(1ULL << bit);
                    bcache_put(be);
                    spin_unlock_irq(&fs_lock, flags);
                    return 0;
                }

                sb.free_blocks--;
                bcache_put(be);
                spin_unlock_irq(&fs_lock, flags);

                /* Zero the allocated block */
                struct bcache_entry *zbe = bcache_get(block_nr);
                if (zbe) {
                    kmemset(zbe->data, 0, 4096);
                    bcache_mark_dirty(zbe);
                    bcache_put(zbe);
                }

                return block_nr;
            }
        }
        bcache_put(be);
    }

    spin_unlock_irq(&fs_lock, flags);
    return 0;
}

void cosmofs_block_free(uint64_t block) {
    if (!mounted || block < sb.data_start || block >= sb.total_blocks) return;

    uint64_t rel = block - sb.data_start;
    uint32_t bitmap_idx = (uint32_t)(rel / (4096 * 8));
    int word_idx = (int)((rel % (4096 * 8)) / 64);
    int bit_idx  = (int)(rel % 64);

    uint64_t flags;
    spin_lock_irq(&fs_lock, &flags);

    struct bcache_entry *be = bcache_get(sb.bitmap_start + bitmap_idx);
    if (be) {
        uint64_t *bits = (uint64_t *)be->data;
        bits[word_idx] &= ~(1ULL << bit_idx);
        bcache_mark_dirty(be);
        bcache_put(be);
        sb.free_blocks++;
    }

    spin_unlock_irq(&fs_lock, flags);
}

/* ── Inode operations ─────────────────────────────── */

/* Temporary inode buffer (static — single-threaded FS access via fs_lock) */
static struct cosmofs_inode inode_tmp;

struct cosmofs_inode *cosmofs_inode_read(uint64_t ino) {
    if (!mounted || ino == 0) return 0;

    uint64_t block_idx = (ino - 1) / COSMOFS_INODES_PER_BLOCK;
    int slot = (int)((ino - 1) % COSMOFS_INODES_PER_BLOCK);

    struct bcache_entry *be = bcache_get(sb.inode_start + block_idx);
    if (!be) return 0;

    kmemcpy(&inode_tmp, be->data + slot * COSMOFS_INODE_SIZE, COSMOFS_INODE_SIZE);
    bcache_put(be);
    return &inode_tmp;
}

int cosmofs_inode_write(uint64_t ino, const struct cosmofs_inode *inode) {
    if (!mounted || ino == 0) return -EINVAL;

    uint64_t block_idx = (ino - 1) / COSMOFS_INODES_PER_BLOCK;
    int slot = (int)((ino - 1) % COSMOFS_INODES_PER_BLOCK);

    struct bcache_entry *be = bcache_get(sb.inode_start + block_idx);
    if (!be) return -EIO;

    kmemcpy(be->data + slot * COSMOFS_INODE_SIZE, inode, COSMOFS_INODE_SIZE);
    bcache_mark_dirty(be);

    /* Log to journal if transaction active */
    if (journal_active())
        journal_write(sb.inode_start + block_idx, be->data);

    bcache_put(be);
    return 0;
}

uint64_t cosmofs_inode_alloc(void) {
    if (!mounted) return 0;

    uint64_t flags;
    spin_lock_irq(&fs_lock, &flags);

    /* Scan inode table for free slot (type == 0) */
    uint64_t max_ino = sb.inode_count;
    for (uint64_t ino = 1; ino <= max_ino; ino++) {
        uint64_t block_idx = (ino - 1) / COSMOFS_INODES_PER_BLOCK;
        int slot = (int)((ino - 1) % COSMOFS_INODES_PER_BLOCK);

        struct bcache_entry *be = bcache_get(sb.inode_start + block_idx);
        if (!be) continue;

        struct cosmofs_inode *ip = (struct cosmofs_inode *)(be->data + slot * COSMOFS_INODE_SIZE);
        if (ip->type == 0) {
            /* Found free inode — mark as allocated */
            kmemset(ip, 0, COSMOFS_INODE_SIZE);
            ip->type = COSMOFS_TYPE_FILE;  /* caller will set actual type */
            ip->ctime = ip->mtime = ip->atime = now_ns();
            bcache_mark_dirty(be);
            bcache_put(be);
            spin_unlock_irq(&fs_lock, flags);
            return ino;
        }
        bcache_put(be);
    }

    spin_unlock_irq(&fs_lock, flags);
    return 0;  /* no free inodes */
}

void cosmofs_inode_free(uint64_t ino) {
    if (!mounted || ino == 0) return;

    uint64_t block_idx = (ino - 1) / COSMOFS_INODES_PER_BLOCK;
    int slot = (int)((ino - 1) % COSMOFS_INODES_PER_BLOCK);

    struct bcache_entry *be = bcache_get(sb.inode_start + block_idx);
    if (!be) return;

    kmemset(be->data + slot * COSMOFS_INODE_SIZE, 0, COSMOFS_INODE_SIZE);
    bcache_mark_dirty(be);
    bcache_put(be);
}

/* ── File data I/O ────────────────────────────────── */

/* Resolve logical file block index to disk block number.
 * Allocates blocks on write if alloc=1. */
static uint64_t resolve_block(struct cosmofs_inode *ip, uint64_t file_block, int alloc) {
    /* Direct blocks */
    if (file_block < COSMOFS_DIRECT_BLOCKS) {
        if (ip->direct[file_block] == 0 && alloc) {
            ip->direct[file_block] = cosmofs_block_alloc();
            if (ip->direct[file_block]) ip->blocks_used++;
        }
        return ip->direct[file_block];
    }

    file_block -= COSMOFS_DIRECT_BLOCKS;

    /* Single indirect: 512 entries */
    if (file_block < 512) {
        if (ip->indirect == 0) {
            if (!alloc) return 0;
            ip->indirect = cosmofs_block_alloc();
            if (!ip->indirect) return 0;
            ip->blocks_used++;
        }
        struct bcache_entry *be = bcache_get(ip->indirect);
        if (!be) return 0;
        uint64_t *ptrs = (uint64_t *)be->data;
        if (ptrs[file_block] == 0 && alloc) {
            ptrs[file_block] = cosmofs_block_alloc();
            if (ptrs[file_block]) ip->blocks_used++;
            bcache_mark_dirty(be);
        }
        uint64_t result = ptrs[file_block];
        bcache_put(be);
        return result;
    }

    file_block -= 512;

    /* Double indirect: 512 × 512 */
    if (file_block < 512ULL * 512) {
        if (ip->double_indirect == 0) {
            if (!alloc) return 0;
            ip->double_indirect = cosmofs_block_alloc();
            if (!ip->double_indirect) return 0;
            ip->blocks_used++;
        }
        int idx1 = (int)(file_block / 512);
        int idx2 = (int)(file_block % 512);

        struct bcache_entry *be1 = bcache_get(ip->double_indirect);
        if (!be1) return 0;
        uint64_t *ptrs1 = (uint64_t *)be1->data;
        if (ptrs1[idx1] == 0 && alloc) {
            ptrs1[idx1] = cosmofs_block_alloc();
            if (ptrs1[idx1]) ip->blocks_used++;
            bcache_mark_dirty(be1);
        }
        uint64_t ind2 = ptrs1[idx1];
        bcache_put(be1);
        if (ind2 == 0) return 0;

        struct bcache_entry *be2 = bcache_get(ind2);
        if (!be2) return 0;
        uint64_t *ptrs2 = (uint64_t *)be2->data;
        if (ptrs2[idx2] == 0 && alloc) {
            ptrs2[idx2] = cosmofs_block_alloc();
            if (ptrs2[idx2]) ip->blocks_used++;
            bcache_mark_dirty(be2);
        }
        uint64_t result = ptrs2[idx2];
        bcache_put(be2);
        return result;
    }

    /* Triple indirect not implemented yet */
    return 0;
}

int cosmofs_read(uint64_t ino, void *buf, size_t offset, size_t len) {
    struct cosmofs_inode *ip = cosmofs_inode_read(ino);
    if (!ip) return -EIO;
    struct cosmofs_inode inode_copy;
    kmemcpy(&inode_copy, ip, sizeof(inode_copy));

    if (offset >= inode_copy.size) return 0;
    if (offset + len > inode_copy.size) len = (size_t)(inode_copy.size - offset);

    uint8_t *dst = (uint8_t *)buf;
    size_t remaining = len;
    size_t pos = offset;

    while (remaining > 0) {
        uint64_t file_block = pos / COSMOFS_BLOCK_SIZE;
        int block_off = (int)(pos % COSMOFS_BLOCK_SIZE);
        int chunk = 4096 - block_off;
        if ((size_t)chunk > remaining) chunk = (int)remaining;

        uint64_t disk_block = resolve_block(&inode_copy, file_block, 0);
        if (disk_block == 0) {
            /* Sparse — read as zeros */
            kmemset(dst, 0, (size_t)chunk);
        } else {
            struct bcache_entry *be = bcache_get(disk_block);
            if (!be) return -EIO;
            kmemcpy(dst, be->data + block_off, (size_t)chunk);
            bcache_put(be);
        }

        dst += chunk;
        pos += (size_t)chunk;
        remaining -= (size_t)chunk;
    }

    return (int)len;
}

int cosmofs_write(uint64_t ino, const void *buf, size_t offset, size_t len) {
    struct cosmofs_inode *ip = cosmofs_inode_read(ino);
    if (!ip) return -EIO;
    struct cosmofs_inode inode_copy;
    kmemcpy(&inode_copy, ip, sizeof(inode_copy));

    const uint8_t *src = (const uint8_t *)buf;
    size_t remaining = len;
    size_t pos = offset;

    journal_begin();

    while (remaining > 0) {
        uint64_t file_block = pos / COSMOFS_BLOCK_SIZE;
        int block_off = (int)(pos % COSMOFS_BLOCK_SIZE);
        int chunk = 4096 - block_off;
        if ((size_t)chunk > remaining) chunk = (int)remaining;

        uint64_t disk_block = resolve_block(&inode_copy, file_block, 1);
        if (disk_block == 0) {
            journal_commit();
            return -ENOMEM;
        }

        struct bcache_entry *be = bcache_get(disk_block);
        if (!be) { journal_commit(); return -EIO; }
        kmemcpy(be->data + block_off, src, (size_t)chunk);
        bcache_mark_dirty(be);
        bcache_put(be);

        src += chunk;
        pos += (size_t)chunk;
        remaining -= (size_t)chunk;
    }

    /* Update inode */
    if (offset + len > inode_copy.size)
        inode_copy.size = offset + len;
    inode_copy.mtime = now_ns();
    cosmofs_inode_write(ino, &inode_copy);

    journal_commit();
    return (int)len;
}

int cosmofs_truncate(uint64_t ino, size_t new_size) {
    struct cosmofs_inode *ip = cosmofs_inode_read(ino);
    if (!ip) return -EIO;
    struct cosmofs_inode inode_copy;
    kmemcpy(&inode_copy, ip, sizeof(inode_copy));

    /* TODO: free blocks beyond new_size */
    inode_copy.size = new_size;
    inode_copy.mtime = now_ns();

    journal_begin();
    cosmofs_inode_write(ino, &inode_copy);
    journal_commit();

    return 0;
}

/* ── Directory operations (B+ tree based) ─────────── */

int cosmofs_dir_lookup(uint64_t dir_ino, const char *name, uint64_t *child_ino) {
    struct cosmofs_inode *ip = cosmofs_inode_read(dir_ino);
    if (!ip || ip->type != COSMOFS_TYPE_DIR) return -ENOTDIR;

    uint64_t root_block = ip->direct[0];
    if (root_block == 0) return -ENOENT;

    return btree_search(root_block, name, child_ino);
}

int cosmofs_dir_create(uint64_t dir_ino, const char *name, uint64_t child_ino) {
    struct cosmofs_inode *ip = cosmofs_inode_read(dir_ino);
    if (!ip || ip->type != COSMOFS_TYPE_DIR) return -ENOTDIR;
    struct cosmofs_inode dir_copy;
    kmemcpy(&dir_copy, ip, sizeof(dir_copy));

    uint64_t root_block = dir_copy.direct[0];

    journal_begin();
    int rc = btree_insert(&root_block, name, child_ino);
    if (rc < 0) { journal_commit(); return rc; }

    dir_copy.direct[0] = root_block;
    dir_copy.mtime = now_ns();
    dir_copy.size++;  /* entry count */
    cosmofs_inode_write(dir_ino, &dir_copy);

    journal_commit();
    return 0;
}

int cosmofs_dir_remove(uint64_t dir_ino, const char *name) {
    struct cosmofs_inode *ip = cosmofs_inode_read(dir_ino);
    if (!ip || ip->type != COSMOFS_TYPE_DIR) return -ENOTDIR;
    struct cosmofs_inode dir_copy;
    kmemcpy(&dir_copy, ip, sizeof(dir_copy));

    uint64_t root_block = dir_copy.direct[0];
    if (root_block == 0) return -ENOENT;

    journal_begin();
    int rc = btree_delete(&root_block, name);
    if (rc < 0) { journal_commit(); return rc; }

    dir_copy.direct[0] = root_block;
    dir_copy.mtime = now_ns();
    if (dir_copy.size > 0) dir_copy.size--;
    cosmofs_inode_write(dir_ino, &dir_copy);

    journal_commit();
    return 0;
}

int cosmofs_dir_rename(uint64_t old_dir, const char *old_name,
                       uint64_t new_dir, const char *new_name) {
    uint64_t child_ino;
    int rc = cosmofs_dir_lookup(old_dir, old_name, &child_ino);
    if (rc < 0) return rc;

    journal_begin();

    /* Remove from old directory */
    struct cosmofs_inode *ip = cosmofs_inode_read(old_dir);
    if (!ip) { journal_commit(); return -EIO; }
    struct cosmofs_inode old_copy;
    kmemcpy(&old_copy, ip, sizeof(old_copy));

    uint64_t old_root = old_copy.direct[0];
    rc = btree_delete(&old_root, old_name);
    if (rc < 0) { journal_commit(); return rc; }
    old_copy.direct[0] = old_root;
    old_copy.mtime = now_ns();
    if (old_copy.size > 0) old_copy.size--;
    cosmofs_inode_write(old_dir, &old_copy);

    /* Add to new directory */
    ip = cosmofs_inode_read(new_dir);
    if (!ip) { journal_commit(); return -EIO; }
    struct cosmofs_inode new_copy;
    kmemcpy(&new_copy, ip, sizeof(new_copy));

    /* Remove existing entry in destination if any */
    uint64_t new_root = new_copy.direct[0];
    uint64_t existing;
    if (new_root != 0 && btree_search(new_root, new_name, &existing) == 0) {
        btree_delete(&new_root, new_name);
        if (new_copy.size > 0) new_copy.size--;
    }

    rc = btree_insert(&new_root, new_name, child_ino);
    if (rc < 0) { journal_commit(); return rc; }
    new_copy.direct[0] = new_root;
    new_copy.mtime = now_ns();
    new_copy.size++;
    cosmofs_inode_write(new_dir, &new_copy);

    journal_commit();
    return 0;
}

int cosmofs_dir_iterate(uint64_t dir_ino, int offset,
                        int (*cb)(const char *, uint64_t, void *), void *ctx) {
    struct cosmofs_inode *ip = cosmofs_inode_read(dir_ino);
    if (!ip || ip->type != COSMOFS_TYPE_DIR) return -ENOTDIR;

    uint64_t root_block = ip->direct[0];
    if (root_block == 0) return 0;

    return btree_iterate(root_block, offset, cb, ctx);
}

/* ── Attributes (stub) ────────────────────────────── */

int cosmofs_attr_set(uint64_t ino, const char *name, const void *val, size_t len) {
    (void)ino; (void)name; (void)val; (void)len;
    return -ENOSYS;
}

int cosmofs_attr_get(uint64_t ino, const char *name, void *val, size_t len) {
    (void)ino; (void)name; (void)val; (void)len;
    return -ENOSYS;
}
