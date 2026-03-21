/* CosmoRT VFS — filesystem dispatch layer
 *
 * Two backends:
 *   VFS_BACKEND_RAM    — in-memory ramfs (original, used for /dev/shm and no-disk boots)
 *   VFS_BACKEND_COSMOFS — persistent CosmoFS on virtio-blk
 *
 * Path routing: CosmoFS is root "/" when mounted. /dev/shm always ramfs.
 * Slab-allocated vfs_node pool, linked-list directories (ramfs only).
 */
#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

/* Node types */
#define VFS_FILE    1
#define VFS_DIR     2
#define VFS_PIPE    3
#define VFS_SYMLINK 4

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
#define S_IFLNK  0120000
#define S_IRWXU  0700
#define S_IRUSR  0400
#define S_IWUSR  0200
#define S_IXUSR  0100

/* Filesystem node (inode equivalent) */
struct vfs_node {
    char name[256];
    int type;               /* VFS_FILE, VFS_DIR, VFS_SYMLINK */
    uint8_t *data;          /* file content (page-allocated) */
    size_t size;            /* current file size */
    size_t capacity;        /* allocated capacity */
    uint64_t ino;           /* inode number */
    uint32_t mode;          /* permission bits */
    uint32_t uid, gid;      /* owner (single-user: always 0) */
    char symlink_target[256]; /* symlink target (VFS_SYMLINK only) */
    struct vfs_node *children;  /* linked list (for directories) */
    struct vfs_node *next;      /* sibling link */
    struct vfs_node *parent;    /* parent directory */
};

/* Filesystem backend */
#define VFS_BACKEND_RAM     0
#define VFS_BACKEND_COSMOFS 1

/* Open file (per-fd state) */
struct vfs_file {
    int type;               /* VFS_FILE, VFS_DIR, VFS_PIPE */
    int flags;              /* O_RDONLY, O_WRONLY, O_RDWR */
    int refcount;           /* reference count (fork shares vfs_file) */
    int backend;            /* VFS_BACKEND_RAM or VFS_BACKEND_COSMOFS */
    uint64_t offset;        /* current read/write position */
    struct vfs_node *node;  /* ramfs node (NULL for cosmofs) */
    uint64_t cosmofs_ino;   /* CosmoFS inode number (0 for ramfs) */
    uint64_t cosmofs_size;  /* cached size for cosmofs files */
    uint64_t cosmofs_dir_ino; /* parent dir inode for cosmofs getdents */
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
int vfs_lstat(const char *path, struct k_stat *buf);
int vfs_fstat(int fd, struct k_stat *buf);

/* Directory operations */
int vfs_getcwd(char *buf, size_t size);
int vfs_chdir(const char *path);

/* Directory/file mutation */
int vfs_mkdir(const char *path);
int vfs_rmdir(const char *path);
int vfs_unlink(const char *path);
int vfs_rename(const char *oldpath, const char *newpath);

/* Symlink operations */
int vfs_symlink(const char *target, const char *linkpath);
int vfs_readlink(const char *path, char *buf, size_t bufsiz);

/* Hard link (returns -ENOSYS if not supported) */
int vfs_link(const char *oldpath, const char *newpath);

/* Metadata operations */
int vfs_chmod(const char *path, uint32_t mode);
int vfs_fchmod(int fd, uint32_t mode);
int vfs_fchown(int fd, uint32_t uid, uint32_t gid);
int vfs_truncate(const char *path, int64_t length);
int vfs_ftruncate(int fd, int64_t length);
int vfs_utimensat(const char *path, const int64_t times[4], int flags);

/* Populate ramfs with a file (for init binary, etc.) */
int vfs_add_file(const char *path, const void *data, size_t len);

/* Mount CosmoFS as root filesystem (called from main after bcache_init) */
void vfs_mount_cosmofs(void);

/* Read from an open file at a specific offset without changing file position.
 * Returns bytes read or negative errno. */
long vfs_pread(struct vfs_file *f, void *buf, size_t count, uint64_t offset);

/* Increment refcount on a vfs_file (for fork fd duplication) */
void vfs_file_incref(struct vfs_file *f);

/* Free a vfs_file object by external pointer (used by proc_cleanup) */
void vfs_file_free_obj(void *obj);

#endif
