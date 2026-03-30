/* CosmoRT VFS — filesystem dispatch layer */
#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

#define __KERNEL__
#include "linux/abi.h"

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
#define S_IROTH  0004
#define S_IWOTH  0002

struct vfs_node {
    char name[256];
    int type;
    uint8_t *data;
    size_t size;
    size_t capacity;
    uint64_t ino;
    uint32_t mode;
    uint32_t uid, gid;
    uint32_t atime, mtime, ctime;
    char symlink_target[256];
    struct vfs_node *children;
    struct vfs_node *next;
    struct vfs_node *parent;
};

#define VFS_BACKEND_RAM     0
#define VFS_BACKEND_EXT2    1

struct vfs_file {
    int type;
    int flags;
    int refcount;
    int backend;
    uint64_t offset;
    struct vfs_node *node;
    uint64_t disk_ino;
    uint64_t disk_size;
    uint64_t disk_dir_ino;
    char path[256];
};

void vfs_init(void);

struct vfs_node *vfs_lookup(const char *path);
struct vfs_node *vfs_create(const char *path, int type);

int vfs_open(const char *path, int flags, int mode);
int vfs_close(int fd);
long vfs_read(int fd, void *buf, size_t count);
long vfs_write(int fd, const void *buf, size_t count);
long vfs_lseek(int fd, long offset, int whence);

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
int vfs_fchmod(int fd, uint32_t mode);
int vfs_fchown(int fd, uint32_t uid, uint32_t gid);
int vfs_truncate(const char *path, int64_t length);
int vfs_ftruncate(int fd, int64_t length);
int vfs_utimensat(const char *path, const int64_t times[4], int flags);

int vfs_add_file(const char *path, const void *data, size_t len);

void vfs_mount_ext2(void);

long vfs_pread(struct vfs_file *f, void *buf, size_t count, uint64_t offset);

long vfs_pwrite(struct vfs_file *f, const void *buf, size_t count, uint64_t offset);

void vfs_file_incref(struct vfs_file *f);

void vfs_file_free_obj(void *obj);

uint64_t vfs_ext2_lookup(const char *path);
int vfs_read_file(const char *path, uint8_t **out_data, size_t *out_size);

#endif
