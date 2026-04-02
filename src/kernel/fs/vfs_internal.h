/* VFS internal declarations — shared across vfs_*.c split files */
#ifndef VFS_INTERNAL_H
#define VFS_INTERNAL_H

#include "fs/vfs.h"
#include "fs/ext2.h"
#include "fs/bcache.h"
#include "event/fd.h"
#include "proc/process.h"
#include "core/percpu.h"
#include "mm/slab.h"
#include "hw/serial.h"
#include "mm/page_alloc.h"
#include "memops.h"
#include "sys/syscall.h"
#include "fs/procfs.h"
#include "event/epoll.h"
#include "vt/pty.h"

/* ── Slab pools (defined in vfs.c) ── */

extern slab_t node_slab;
extern slab_t inode_slab;
extern slab_t file_slab;
extern struct vfs_node *vfs_root_node;
extern uint64_t vfs_next_ino;

/* ── String helpers (defined in vfs.c) ── */

int kstrlen(const char *s);
void kstrncpy(char *dst, const char *src, int max);
int kstreq(const char *a, const char *b);

/* ── Routing helpers (defined in vfs.c) ── */

const char *procfs_name(const char *path);
int is_ramfs_path(const char *path);
uint64_t ext2_walk(const char *path);
uint64_t ext2_walk_err(const char *path, int *err);
uint64_t ext2_walk_parent(const char *path, const char **basename_out);
uint64_t ext2_walk_parent_err(const char *path, const char **basename_out, int *err);
char *vfs_get_cwd(void);

/* ── Node/file allocation (defined in vfs.c) ── */

struct vfs_node *node_alloc(const char *name, int type);
void node_destroy(struct vfs_node *node);
void inode_destroy(struct vfs_inode *ino);
void inode_decref(struct vfs_inode *ino);
struct vfs_file *file_alloc(void);
void file_free(struct vfs_file *f);
int ext2_open_count(uint32_t ino);
void ext2_open_inc(uint32_t ino);
int ext2_open_dec(uint32_t ino);

/* ── Path operations (defined in vfs_lookup.c) ── */

struct vfs_node *vfs_lookup_nofollow(const char *path, int *err);
struct vfs_node *lookup_parent(const char *path, const char **basename);
struct vfs_node *ensure_dirs(const char *path);

/* ── File growth (defined in vfs_rw.c) ── */

int grow_file(struct vfs_inode *inode, size_t needed);

/* ── Inotify helper (defined in vfs.c) ── */

void vfs_notify_modify(struct vfs_node *node);

/* ── Dir mutation helper (defined in vfs_dirops.c) ── */

int unlink_child(struct vfs_node *parent, struct vfs_node *child);

/* ── Stat helpers (defined in vfs_ioctls.c) ── */

void fill_stat(struct vfs_inode *inode, struct k_stat *buf);
void fill_ext2_stat(uint32_t ino, struct ext2_inode *ip, struct k_stat *buf);

/* ── Device file IDs ── */

#define DEV_NULL    1
#define DEV_ZERO    2
#define DEV_URANDOM 3
#define DEV_TTY     4

#endif
