/* CosmoRT ext4 — read-write ext4 filesystem driver
 *
 * On-disk format (ext4 spec):
 *   Block 0: Boot block (skipped, 1024 bytes)
 *   Bytes 1024..2047: Superblock
 *   Block group descriptors follow superblock
 *   Each block group: block bitmap, inode bitmap, inode table, data blocks
 *
 * Limitations: no triple indirect, no xattr, no journal (ext4 only).
 */
#ifndef EXT4_H
#define EXT4_H

#include <stdint.h>
#include <stddef.h>

/* ── On-disk superblock (bytes 1024..2047) ───────── */

#define EXT4_MAGIC 0xEF53

struct ext4_super {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;       /* block_size = 1024 << s_log_block_size */
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* EXT4_DYNAMIC_REV fields */
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    /* rest not needed */
} __attribute__((packed));

/* ── Block group descriptor ──────────────────────── */

struct ext4_group_desc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed));

/* ── On-disk inode ───────────────────────────────── */

#define EXT4_NDIR_BLOCKS  12
#define EXT4_IND_BLOCK    12
#define EXT4_DIND_BLOCK   13
#define EXT4_TIND_BLOCK   14
#define EXT4_N_BLOCKS     15

#define EXT4_S_IFMT   0xF000
#define EXT4_S_IFSOCK 0xC000
#define EXT4_S_IFLNK  0xA000
#define EXT4_S_IFREG  0x8000
#define EXT4_S_IFBLK  0x6000
#define EXT4_S_IFDIR  0x4000
#define EXT4_S_IFCHR  0x2000
#define EXT4_S_IFIFO  0x1000

struct ext4_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;           /* lower 32 bits */
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;         /* 512-byte sectors */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[EXT4_N_BLOCKS];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;        /* i_size_high for regular files (rev >= 1) */
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed));

_Static_assert(sizeof(struct ext4_inode) == 128, "ext4_inode must be 128 bytes");

/* Extra inode fields (at offset 128 when inode_size >= 256).
 * Layout matches Linux ext4 for compatibility. */
struct ext4_inode_extra {
    uint16_t i_extra_isize;   /* size of this extra area */
    uint16_t i_checksum_hi;
    uint32_t i_ctime_extra;   /* bits 0-1: epoch (upper 2 bits of sec), 2-31: nsec */
    uint32_t i_mtime_extra;
    uint32_t i_atime_extra;
    uint32_t i_crtime;        /* creation time (lower 32 bits) */
    uint32_t i_crtime_extra;
} __attribute__((packed));

/* Encode/decode 64-bit timestamp from base + extra fields.
 * extra bits 0-1 = upper 2 bits of seconds (34-bit total).
 * extra bits 2-31 = nanoseconds (unused by us, always 0). */
static inline uint64_t ext4_decode_ts(uint32_t base, uint32_t extra) {
    return (uint64_t)base | ((uint64_t)(extra & 3) << 32);
}
static inline void ext4_encode_ts(uint64_t ts, uint32_t *base, uint32_t *extra) {
    *base = (uint32_t)ts;
    *extra = (uint32_t)((ts >> 32) & 3);
}

/* Check if extra timestamp fields are available */
int ext4_has_extra_isize(void);
int ext4_read_extra(uint32_t ino, struct ext4_inode_extra *out);
int ext4_write_extra(uint32_t ino, const struct ext4_inode_extra *in);

/* ── Directory entry ─────────────────────────────── */

#define EXT4_FT_UNKNOWN  0
#define EXT4_FT_REG_FILE 1
#define EXT4_FT_DIR      2
#define EXT4_FT_CHRDEV   3
#define EXT4_FT_BLKDEV   4
#define EXT4_FT_FIFO     5
#define EXT4_FT_SOCK     6
#define EXT4_FT_SYMLINK  7

struct ext4_dir_entry_2 {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];           /* variable length, NOT null-terminated */
} __attribute__((packed));

/* ── Root inode ──────────────────────────────────── */

#define EXT4_ROOT_INO 2

/* ── API ──────────────────────────────────────────── */

/* Mount/unmount */
int ext4_mount(void);
int ext4_unmount(void);
int ext4_mounted(void);

/* Inode operations */
int ext4_inode_read(uint32_t ino, struct ext4_inode *out);
int ext4_inode_write(uint32_t ino, const struct ext4_inode *in);

/* File data I/O */
int ext4_read(uint32_t ino, void *buf, size_t offset, size_t len);
int ext4_write(uint32_t ino, const void *buf, size_t offset, size_t len);
int ext4_truncate(uint32_t ino, size_t new_size);

/* Directory operations */
int ext4_dir_lookup(uint32_t dir_ino, const char *name, uint32_t *child_ino);
int ext4_dir_add(uint32_t dir_ino, const char *name, uint32_t child_ino, uint8_t file_type);
int ext4_dir_remove(uint32_t dir_ino, const char *name);
int ext4_dir_iterate(uint32_t dir_ino, uint32_t byte_offset,
                     int (*cb)(const char *name, uint32_t ino, uint8_t type,
                               uint32_t next_pos, void *ctx),
                     void *ctx);

/* Allocation */
uint32_t ext4_inode_alloc(int is_dir);
void ext4_inode_free(uint32_t ino);
uint32_t ext4_block_alloc(void);
void ext4_block_free(uint32_t block);

/* High-level create/delete */
int ext4_create(uint32_t parent_ino, const char *name, uint16_t mode, uint32_t *new_ino);
int ext4_mkdir(uint32_t parent_ino, const char *name, uint16_t mode, uint32_t *new_ino);
int ext4_symlink_create(uint32_t parent_ino, const char *name, const char *target);

/* Symlink reading */
int ext4_readlink(uint32_t ino, char *buf, size_t bufsiz);

/* Rename */
int ext4_rename(uint32_t old_parent, const char *old_name,
                uint32_t new_parent, const char *new_name);

/* Root inode (always 2 for ext4) */
static inline uint32_t ext4_root_ino(void) { return EXT4_ROOT_INO; }

/* Get cached block size */
uint32_t ext4_block_size(void);

/* Sync (write superblock + dirty blocks) */
void ext4_sync(void);

#endif
