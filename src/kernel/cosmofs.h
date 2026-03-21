/* CosmoRT CosmoFS — BFS-inspired persistent filesystem
 *
 * On-disk format:
 *   Block 0: Superblock
 *   Block 1..N: Free-block bitmap
 *   Block N+1..M: Inode table
 *   Block M+1..J: Journal
 *   Block J+1..: Data blocks
 *
 * Inode: 256 bytes (16 per 4KB block).
 * Directories: B+ tree (key=name, value=inode number).
 * Attribute indices: B+ tree (per-inode, key=attr name, value=block).
 */
#ifndef COSMOFS_H
#define COSMOFS_H

#include <stdint.h>
#include <stddef.h>

/* ── On-disk superblock ───────────────────────────── */

#define COSMOFS_MAGIC       "CosmoFS\0"
#define COSMOFS_MAGIC_U64   0x0053466F6D736F43ULL  /* "CosmoFS\0" as LE uint64 */
#define COSMOFS_VERSION     1
#define COSMOFS_BLOCK_SIZE  4096
#define COSMOFS_INODE_SIZE  256
#define COSMOFS_INODES_PER_BLOCK (COSMOFS_BLOCK_SIZE / COSMOFS_INODE_SIZE)

struct cosmofs_super {
    uint64_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t bitmap_start;      /* first bitmap block */
    uint32_t bitmap_blocks;     /* number of bitmap blocks */
    uint32_t _pad0;
    uint64_t inode_start;       /* first inode table block */
    uint32_t inode_blocks;      /* number of inode table blocks */
    uint32_t _pad1;
    uint64_t inode_count;       /* total inodes */
    uint64_t root_inode;        /* inode number of root directory */
    uint64_t journal_start;     /* first journal block */
    uint32_t journal_size;      /* journal size in blocks */
    uint32_t _pad2;
    uint64_t data_start;        /* first data block */
} __attribute__((packed));

/* ── On-disk inode ────────────────────────────────── */

#define COSMOFS_TYPE_FILE    1
#define COSMOFS_TYPE_DIR     2
#define COSMOFS_TYPE_SYMLINK 3
#define COSMOFS_TYPE_DEVICE  4

#define COSMOFS_DIRECT_BLOCKS 12

struct cosmofs_inode {
    uint16_t type;
    uint16_t flags;
    uint32_t uid;
    uint32_t gid;
    uint32_t _pad0;
    uint64_t size;
    uint64_t blocks_used;
    uint64_t ctime;             /* nanoseconds since epoch */
    uint64_t mtime;
    uint64_t atime;
    uint64_t direct[COSMOFS_DIRECT_BLOCKS];   /* 12 × 4KB = 48KB */
    uint64_t indirect;          /* 512 entries → 2MB */
    uint64_t double_indirect;   /* 512² → 1GB */
    uint64_t triple_indirect;   /* 512³ → 512GB */
    uint64_t attr_block;        /* B+ tree root for attributes */
    uint64_t stream_block;      /* named sub-streams */
    uint64_t reserved[8];
} __attribute__((packed));

/* Verify inode size at compile time */
_Static_assert(sizeof(struct cosmofs_inode) == 256, "inode must be 256 bytes");

/* ── API ──────────────────────────────────────────── */

/* Mount/unmount */
int cosmofs_mount(void);
int cosmofs_unmount(void);
int cosmofs_mounted(void);

/* Inode operations */
struct cosmofs_inode *cosmofs_inode_read(uint64_t ino);
int cosmofs_inode_write(uint64_t ino, const struct cosmofs_inode *inode);
uint64_t cosmofs_inode_alloc(void);
void cosmofs_inode_free(uint64_t ino);

/* Block allocation (used by btree.c) */
uint64_t cosmofs_block_alloc(void);
void cosmofs_block_free(uint64_t block);

/* File data I/O */
int cosmofs_read(uint64_t ino, void *buf, size_t offset, size_t len);
int cosmofs_write(uint64_t ino, const void *buf, size_t offset, size_t len);
int cosmofs_truncate(uint64_t ino, size_t new_size);

/* Directory operations */
int cosmofs_dir_lookup(uint64_t dir_ino, const char *name, uint64_t *child_ino);
int cosmofs_dir_create(uint64_t dir_ino, const char *name, uint64_t child_ino);
int cosmofs_dir_remove(uint64_t dir_ino, const char *name);
int cosmofs_dir_rename(uint64_t old_dir, const char *old_name,
                       uint64_t new_dir, const char *new_name);
int cosmofs_dir_iterate(uint64_t dir_ino, int offset,
                        int (*cb)(const char *, uint64_t, void *), void *ctx);

/* Attributes (stub) */
int cosmofs_attr_set(uint64_t ino, const char *name, const void *val, size_t len);
int cosmofs_attr_get(uint64_t ino, const char *name, void *val, size_t len);

/* Get root inode number */
uint64_t cosmofs_root_ino(void);

#endif
