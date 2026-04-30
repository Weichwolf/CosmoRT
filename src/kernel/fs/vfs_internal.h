/* VFS internal declarations — shared across vfs_*.c split files */
#ifndef VFS_INTERNAL_H
#define VFS_INTERNAL_H

#include "fs/vfs.h"
#include "fs/ext4.h"
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
uint64_t ext4_walk(const char *path);
uint64_t ext4_walk_err(const char *path, int *err);
uint64_t ext4_walk_parent(const char *path, const char **basename_out);
uint64_t ext4_walk_parent_err(const char *path, const char **basename_out, int *err);
/* Per-instance variants for loop-mounts. fs==NULL routes to default. */
uint64_t ext4_walk_inst_err(struct ext4_fs *fs, const char *path, int *err);
uint64_t ext4_walk_parent_inst_err(struct ext4_fs *fs, const char *path,
                                   const char **basename_out, int *err);
char *vfs_get_cwd(void);

/* ── Node/file allocation (defined in vfs.c) ── */

struct vfs_node *node_alloc(const char *name, int type);
void node_destroy(struct vfs_node *node);
void inode_destroy(struct vfs_inode *ino);
void inode_decref(struct vfs_inode *ino);
struct vfs_file *file_alloc(void);
void file_free(struct vfs_file *f);
int ext4_open_count(uint32_t ino);
void ext4_open_inc(uint32_t ino);
int ext4_open_dec(uint32_t ino);

/* ── Path operations (defined in vfs_lookup.c) ── */

struct vfs_node *vfs_lookup_nofollow(const char *path, int *err);
struct vfs_node *lookup_parent(const char *path, const char **basename);
struct vfs_node *ensure_dirs(const char *path);

/* ── File growth / page-list helpers (defined in vfs_rw.c) ──
 *
 * grow_file:   ensure inode has at least `needed` bytes capacity.
 *              Pages are 4KB each, individually allocated.
 *              Returns 0 on success, -ENOMEM on failure (no partial allocation).
 * shrink_file: drop pages above `new_size`. Caller adjusts inode->size.
 * ramfs_read / ramfs_write: flat r/w through the page-list.
 *              Caller must hold inode->lock OR ensure no concurrent grow/shrink.
 *              For locked variants see ramfs_read_locked / _write_locked. */
int grow_file(struct vfs_inode *inode, size_t needed);
void shrink_file(struct vfs_inode *inode, size_t new_size);
void ramfs_read(struct vfs_inode *inode, void *dst, size_t off, size_t len);
void ramfs_write(struct vfs_inode *inode, const void *src, size_t off, size_t len);
void ramfs_zero(struct vfs_inode *inode, size_t off, size_t len);
/* Convenience locked variants — for callers outside vfs_rw.c / tmpfs.c */
size_t ramfs_read_locked(struct vfs_inode *inode, void *dst, size_t off, size_t len);

/* ── Inotify helper (defined in vfs.c) ── */

void vfs_notify_modify(struct vfs_node *node);

/* ── Dir mutation helper (defined in vfs_dirops.c) ── */

int unlink_child(struct vfs_node *parent, struct vfs_node *child);

/* ── Stat helpers (defined in vfs_ioctls.c) ── */

void fill_stat(struct vfs_inode *inode, struct k_stat *buf);
void fill_ext4_stat(uint32_t ino, struct ext4_inode *ip, struct k_stat *buf);

/* ── Device file IDs ── */

#define DEV_NULL    1
#define DEV_ZERO    2
#define DEV_URANDOM 3
#define DEV_TTY     4

#endif
