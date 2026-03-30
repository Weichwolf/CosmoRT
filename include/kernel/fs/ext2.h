/* CosmoRT ext2 — read-write ext2 filesystem driver */
#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>
#include <stddef.h>

#define EXT2_MAGIC 0xEF53

struct ext2_super {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
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
} __attribute__((packed));

struct ext2_group_desc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed));

#define EXT2_NDIR_BLOCKS  12
#define EXT2_IND_BLOCK    12
#define EXT2_DIND_BLOCK   13
#define EXT2_TIND_BLOCK   14
#define EXT2_N_BLOCKS     15

#define EXT2_S_IFMT   0xF000
#define EXT2_S_IFSOCK 0xC000
#define EXT2_S_IFLNK  0xA000
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFBLK  0x6000
#define EXT2_S_IFDIR  0x4000
#define EXT2_S_IFCHR  0x2000
#define EXT2_S_IFIFO  0x1000

struct ext2_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[EXT2_N_BLOCKS];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed));

_Static_assert(sizeof(struct ext2_inode) == 128, "ext2_inode must be 128 bytes");

#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_CHRDEV   3
#define EXT2_FT_BLKDEV   4
#define EXT2_FT_FIFO     5
#define EXT2_FT_SOCK     6
#define EXT2_FT_SYMLINK  7

struct ext2_dir_entry_2 {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} __attribute__((packed));

#define EXT2_ROOT_INO 2

int ext2_mount(void);
int ext2_unmount(void);
int ext2_mounted(void);

int ext2_inode_read(uint32_t ino, struct ext2_inode *out);
int ext2_inode_write(uint32_t ino, const struct ext2_inode *in);

int ext2_read(uint32_t ino, void *buf, size_t offset, size_t len);
int ext2_write(uint32_t ino, const void *buf, size_t offset, size_t len);
int ext2_truncate(uint32_t ino, size_t new_size);

int ext2_dir_lookup(uint32_t dir_ino, const char *name, uint32_t *child_ino);
int ext2_dir_add(uint32_t dir_ino, const char *name, uint32_t child_ino, uint8_t file_type);
int ext2_dir_remove(uint32_t dir_ino, const char *name);
int ext2_dir_iterate(uint32_t dir_ino, uint32_t byte_offset,
                     int (*cb)(const char *name, uint32_t ino, uint8_t type,
                               uint32_t next_pos, void *ctx),
                     void *ctx);

uint32_t ext2_inode_alloc(int is_dir);
void ext2_inode_free(uint32_t ino);
uint32_t ext2_block_alloc(void);
void ext2_block_free(uint32_t block);

int ext2_create(uint32_t parent_ino, const char *name, uint16_t mode, uint32_t *new_ino);
int ext2_mkdir(uint32_t parent_ino, const char *name, uint16_t mode, uint32_t *new_ino);
int ext2_symlink_create(uint32_t parent_ino, const char *name, const char *target);

int ext2_readlink(uint32_t ino, char *buf, size_t bufsiz);

int ext2_rename(uint32_t old_parent, const char *old_name,
                uint32_t new_parent, const char *new_name);

static inline uint32_t ext2_root_ino(void) { return EXT2_ROOT_INO; }

uint32_t ext2_block_size(void);

void ext2_sync(void);

#endif
