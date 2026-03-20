/* CosmoRT VFS — minimal in-memory filesystem (ramfs)
 *
 * No disk, no persistence. Files live in page-allocated RAM.
 * Slab-allocated vfs_node pool, linked-list directories.
 */
#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

/* Node types */
#define VFS_FILE    1
#define VFS_DIR     2
#define VFS_PIPE    3

/* Open flags (Linux-compatible) */
#ifndef O_RDONLY
#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#endif
#define O_CREAT    0x0040
#define O_TRUNC    0x0200
#define O_APPEND   0x0400
#define O_CLOEXEC  0x80000
#define O_DIRECTORY 0x10000

/* Seek */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* Stat struct — Linux x86_64 layout */
struct k_stat {
    uint64_t st_dev, st_ino;
    uint64_t st_nlink;
    uint32_t st_mode, st_uid, st_gid, __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize, st_blocks;
    int64_t  st_atime_sec, st_atime_nsec;
    int64_t  st_mtime_sec, st_mtime_nsec;
    int64_t  st_ctime_sec, st_ctime_nsec;
    int64_t  __unused[3];
};

/* st_mode bits */
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFIFO  0010000
#define S_IRWXU  0700
#define S_IRUSR  0400
#define S_IWUSR  0200
#define S_IXUSR  0100

/* Filesystem node (inode equivalent) */
struct vfs_node {
    char name[256];
    int type;               /* VFS_FILE or VFS_DIR */
    uint8_t *data;          /* file content (page-allocated) */
    size_t size;            /* current file size */
    size_t capacity;        /* allocated capacity */
    uint64_t ino;           /* inode number */
    struct vfs_node *children;  /* linked list (for directories) */
    struct vfs_node *next;      /* sibling link */
    struct vfs_node *parent;    /* parent directory */
};

/* Open file (per-fd state) */
struct vfs_file {
    int type;               /* VFS_FILE, VFS_DIR, VFS_PIPE */
    int flags;              /* O_RDONLY, O_WRONLY, O_RDWR */
    uint64_t offset;        /* current read/write position */
    struct vfs_node *node;
};

/* Initialize VFS — create root directory "/" */
void vfs_init(void);

/* Path operations */
struct vfs_node *vfs_lookup(const char *path);
struct vfs_node *vfs_create(const char *path, int type);

/* File operations (take process fd_table, return fd or error) */
int vfs_open(const char *path, int flags, int mode);
int vfs_close(int fd);
long vfs_read(int fd, void *buf, size_t count);
long vfs_write(int fd, const void *buf, size_t count);
long vfs_lseek(int fd, long offset, int whence);

/* Stat operations */
int vfs_stat(const char *path, struct k_stat *buf);
int vfs_fstat(int fd, struct k_stat *buf);

/* Directory operations */
int vfs_getcwd(char *buf, size_t size);
int vfs_chdir(const char *path);

/* Populate ramfs with a file (for init binary, etc.) */
int vfs_add_file(const char *path, const void *data, size_t len);

#endif
