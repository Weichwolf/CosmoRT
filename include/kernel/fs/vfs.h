/* CosmoRT VFS — Virtual Filesystem Switch
 *
 * Like Linux: super_operations + inode_operations + file_operations.
 *
 * Filesystems register via vfs_mount(). VFS dispatches through mount table
 * (longest-prefix match). Each FS implements three operation tables:
 *
 *   super_ops  — per-mount: sync, statfs
 *   inode_ops  — per-directory: lookup, create, mkdir, unlink, symlink
 *   file_ops   — per-open-file: read, write, lseek, getdents, poll
 *
 * Path resolution walks component-by-component. At each directory, VFS calls
 * inode_ops->lookup(dir, name) to resolve the next component. Crossing a
 * mount boundary switches to the mounted FS's root.
 *
 * Mount table:
 *   /         ext4
 *   /dev      devfs
 *   /dev/shm  tmpfs
 *   /proc     procfs
 *   /tmp      tmpfs
 */
#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

#define __KERNEL__
#include "linux/abi.h"

/* ── Node types ─────────────────────────────────── */

#define VFS_FILE    1
#define VFS_DIR     2
#define VFS_PIPE    3
#define VFS_SYMLINK 4

#define S_IRWXU  0700
#define S_IRUSR  0400
#define S_IWUSR  0200
#define S_IXUSR  0100
#define S_IRGRP  0040
#define S_IWGRP  0020
#define S_IXGRP  0010
#define S_IROTH  0004
#define S_IWOTH  0002
#define S_IXOTH  0001

/* ── VFS inode — unified across all filesystems ─── */

struct vfs_inode {
    int type;
    uint8_t *data;          /* tmpfs: file content */
    size_t size;
    size_t capacity;
    uint64_t ino;
    uint32_t mode;          /* permission bits (07777 + suid/sgid/sticky); type separate */
    uint32_t uid, gid;
    uint32_t nlink;
    uint64_t atime, mtime, ctime;
    char symlink_target[256];
    int refcount;           /* in-memory refs (open fds + dentries); not POSIX nlink */
    struct vfs_node *children;  /* tmpfs dir entries */
};

/* ── Dentry ─────────────────────────────────────── */

struct vfs_node {
    char name[256];
    struct vfs_inode *inode;
    struct vfs_node *next;
    struct vfs_node *parent;
};

/* Legacy backend IDs — will be removed after full fs_ops migration */
#define VFS_BACKEND_RAM     0
#define VFS_BACKEND_EXT4    1

/* ── Forward declarations ───────────────────────── */

struct vfs_file;
struct mount;

/* ── Super operations (per-mount) ───────────────── */

struct super_ops {
    void (*sync)(struct mount *mnt);
};

/* ── Inode operations (namespace: lookup/create/delete) ── */

struct inode_ops {
    /* Resolve full relative path within this FS.
     * Returns fs-specific handle (ext4: ino, tmpfs: vfs_node*) via out param.
     * Sets *err to specific errno on failure.
     * follow: 1=follow final symlink, 0=nofollow (lstat/readlink). */
    int (*lookup)(struct mount *mnt, const char *relpath, int follow,
                  uint64_t *handle, int *err);

    int (*stat)(struct mount *mnt, const char *relpath, struct k_stat *buf);
    int (*lstat)(struct mount *mnt, const char *relpath, struct k_stat *buf);

    int (*create)(struct mount *mnt, const char *relpath, int mode);
    int (*mkdir)(struct mount *mnt, const char *relpath, int mode);
    int (*rmdir)(struct mount *mnt, const char *relpath);
    int (*unlink)(struct mount *mnt, const char *relpath);
    int (*rename)(struct mount *mnt, const char *oldrel, const char *newrel);

    int (*symlink)(struct mount *mnt, const char *target, const char *relpath);
    int (*readlink)(struct mount *mnt, const char *relpath, char *buf, size_t bufsiz);
    int (*link)(struct mount *mnt, const char *oldrel, const char *newrel);

    int (*chmod)(struct mount *mnt, const char *relpath, uint32_t mode);
    int (*chown)(struct mount *mnt, const char *relpath, uint32_t uid, uint32_t gid);
    int (*lchown)(struct mount *mnt, const char *relpath, uint32_t uid, uint32_t gid);
    int (*truncate)(struct mount *mnt, const char *relpath, int64_t length);
    int (*utimensat)(struct mount *mnt, const char *relpath,
                     const int64_t times[4], int flags);
};

/* ── File operations (per-open-file) ────────────── */

struct file_ops {
    long (*read)(struct vfs_file *f, void *buf, size_t count);
    long (*write)(struct vfs_file *f, const void *buf, size_t count);
    long (*lseek)(struct vfs_file *f, long offset, int whence);
    long (*pread)(struct vfs_file *f, void *buf, size_t count, uint64_t offset);
    long (*pwrite)(struct vfs_file *f, const void *buf, size_t count, uint64_t offset);
    int  (*close)(struct vfs_file *f);
    int  (*getdents)(struct vfs_file *f, void *buf, size_t count);
    int  (*fstat)(struct vfs_file *f, struct k_stat *buf);
    int  (*ftruncate)(struct vfs_file *f, int64_t length);
    int  (*fchmod)(struct vfs_file *f, uint32_t mode);
    int  (*fchown)(struct vfs_file *f, uint32_t uid, uint32_t gid);
    int  (*fsync)(struct vfs_file *f);
};

/* ── Mount entry ────────────────────────────────── */

#define MOUNT_MAX 16

struct mount {
    const char *path;
    int pathlen;
    struct super_ops *s_ops;
    struct inode_ops *i_ops;
    struct file_ops  *f_ops;    /* default file_ops (FS can override per-file) */
    void *fs_data;
};

/* ── Open file (per-fd state) ───────────────────── */

struct vfs_file {
    int type;
    int flags;
    int refcount;
    uint64_t offset;
    struct file_ops *f_ops;     /* file_ops for this open file */
    struct mount *mnt;          /* owning mount */
    char path[256];

    /* Legacy backend tag — will be removed when all fs_ops are implemented */
    int backend;
    /* Backend-specific storage */
    struct vfs_inode *inode;    /* tmpfs */
    uint64_t disk_ino;          /* ext4 */
    uint64_t disk_size;         /* ext4 cached size */
    uint64_t disk_dir_ino;      /* ext4 dir iteration */
};

/* ── Mount table API ────────────────────────────── */

void vfs_init(void);
int  vfs_mount(const char *path, struct super_ops *s_ops,
               struct inode_ops *i_ops, struct file_ops *f_ops, void *fs_data);
struct mount *vfs_resolve_mount(const char *abspath, const char **relpath);

/* ── Path operations (tmpfs dentry tree) ────────── */

struct vfs_node *vfs_lookup(const char *path);
struct vfs_node *vfs_lookup_err(const char *path, int *err);
struct vfs_node *vfs_lookup_nofollow(const char *path, int *err);
struct vfs_node *vfs_create(const char *path, int type);

/* ── Syscall-facing API ─────────────────────────── */

int  vfs_open(const char *path, int flags, int mode);
int  vfs_close(int fd);
long vfs_read(int fd, void *buf, size_t count);
long vfs_write(int fd, const void *buf, size_t count);
long vfs_lseek(int fd, long offset, int whence);
long vfs_pread(struct vfs_file *f, void *buf, size_t count, uint64_t offset);
long vfs_pwrite(struct vfs_file *f, const void *buf, size_t count, uint64_t offset);

int vfs_stat(const char *path, struct k_stat *buf);
int vfs_lstat(const char *path, struct k_stat *buf);
int vfs_fstat(int fd, struct k_stat *buf);

int vfs_getcwd(char *buf, size_t size);
int vfs_chdir(const char *path);

int vfs_mkdir(const char *path);
int vfs_rmdir(const char *path);
int vfs_unlink(const char *path);
int vfs_rename(const char *oldpath, const char *newpath);

int vfs_symlink(const char *target, const char *linkpath);
int vfs_readlink(const char *path, char *buf, size_t bufsiz);
int vfs_link(const char *oldpath, const char *newpath);

int vfs_chmod(const char *path, uint32_t mode);
int vfs_chown(const char *path, uint32_t uid, uint32_t gid);
int vfs_lchown(const char *path, uint32_t uid, uint32_t gid);
int vfs_fchmod(int fd, uint32_t mode);
int vfs_fchown(int fd, uint32_t uid, uint32_t gid);
int vfs_truncate(const char *path, int64_t length);
int vfs_ftruncate(int fd, int64_t length);
int vfs_utimensat(const char *path, const int64_t times[4], int flags);

/* ── Utility ────────────────────────────────────── */

int  vfs_add_file(const char *path, const void *data, size_t len);
void vfs_mount_ext4(void);
int  vfs_read_file(const char *path, uint8_t **out_data, size_t *out_size);
long vfs_pread_by_ino(int backend, uint64_t ino, void *buf, size_t offset, size_t len);
long vfs_pwrite_by_ino(int backend, uint64_t ino, const void *buf, size_t offset, size_t len);
void vfs_file_incref(struct vfs_file *f);
void vfs_file_free_obj(void *obj);
uint64_t vfs_ext4_lookup(const char *path);

/* ── Filesystem registrations ───────────────────── */

extern struct inode_ops ext4_inode_ops;
extern struct file_ops  ext4_file_ops;
extern struct super_ops ext4_super_ops;

extern struct inode_ops tmpfs_inode_ops;
extern struct file_ops  tmpfs_file_ops;

extern struct inode_ops procfs_inode_ops;
extern struct file_ops  procfs_file_ops;

extern struct inode_ops devfs_inode_ops;
extern struct file_ops  devfs_file_ops;

#endif
