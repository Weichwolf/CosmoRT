/* CosmoRT ext2 filesystem — VFS ops wrapper
 *
 * Wraps the low-level ext2.h API (inode_read, dir_lookup, etc.)
 * into VFS inode_ops / file_ops for mount-table dispatch.
 */

#include "fs/vfs.h"
#include "fs/ext2.h"
#include "fs/bcache.h"
#include "hw/serial.h"
#include "memops.h"

/* Forward declarations for helpers defined in vfs.c (will migrate later) */
extern uint64_t ext2_walk_err(const char *path, int *err);
extern uint64_t ext2_walk_parent_err(const char *path, const char **basename, int *err);
extern void fill_ext2_stat(uint32_t ino, struct ext2_inode *ip, struct k_stat *buf);
extern int ext2_open_count(uint32_t ino);
extern void ext2_open_inc(uint32_t ino);
extern int ext2_open_dec(uint32_t ino);
extern void inotify_event(const char *path, uint32_t mask);
extern void page_cache_invalidate_ino(uint64_t ino);
extern uint32_t timer_epoch_sec(void);

#define NAME_MAX 255

/* ── inode_ops ──────────────────────────────────── */

static int ext2_vfs_lookup(struct mount *mnt, const char *relpath, int follow,
                           uint64_t *handle, int *err) {
    (void)mnt; (void)follow;
    uint64_t ino = ext2_walk_err(relpath, err);
    if (ino == 0) return -1;
    *handle = ino;
    return 0;
}

static int ext2_vfs_stat(struct mount *mnt, const char *relpath, struct k_stat *buf) {
    (void)mnt;
    int err = -ENOENT;
    uint64_t ino64 = ext2_walk_err(relpath, &err);
    uint32_t ino = (uint32_t)ino64;
    if (ino == 0) return err;
    struct ext2_inode ip;
    if (ext2_inode_read(ino, &ip) < 0) return -EIO;
    fill_ext2_stat(ino, &ip, buf);
    return 0;
}

static int ext2_vfs_lstat(struct mount *mnt, const char *relpath, struct k_stat *buf) {
    (void)mnt;
    /* Walk to parent, lookup name without following symlink */
    const char *basename;
    int perr = -ENOENT;
    uint64_t parent_ino64 = ext2_walk_parent_err(relpath, &basename, &perr);
    uint32_t parent_ino = (uint32_t)parent_ino64;
    if (parent_ino == 0) {
        if (relpath[0] == '/' && relpath[1] == 0) {
            struct ext2_inode ip;
            if (ext2_inode_read(EXT2_ROOT_INO, &ip) < 0) return -EIO;
            fill_ext2_stat(EXT2_ROOT_INO, &ip, buf);
            return 0;
        }
        return perr;
    }
    uint32_t ino;
    if (ext2_dir_lookup(parent_ino, basename, &ino) < 0) return -ENOENT;
    struct ext2_inode ip;
    if (ext2_inode_read(ino, &ip) < 0) return -EIO;
    fill_ext2_stat(ino, &ip, buf);
    return 0;
}

static int ext2_vfs_mkdir(struct mount *mnt, const char *relpath, int mode) {
    (void)mnt;
    /* Check if exists */
    uint64_t ino64 = ext2_walk_err(relpath, 0);
    if (ino64 != 0) return -EEXIST;

    const char *basename;
    int perr = -ENOENT;
    uint64_t parent_ino64 = ext2_walk_parent_err(relpath, &basename, &perr);
    uint32_t parent_ino = (uint32_t)parent_ino64;
    if (parent_ino == 0) return perr;

    uint32_t new_ino;
    return ext2_mkdir(parent_ino, basename, (uint16_t)(mode & 07777), &new_ino);
}

static int ext2_vfs_rmdir(struct mount *mnt, const char *relpath) {
    (void)mnt;
    const char *basename;
    int perr = -ENOENT;
    uint64_t parent_ino64 = ext2_walk_parent_err(relpath, &basename, &perr);
    uint32_t parent_ino = (uint32_t)parent_ino64;
    if (parent_ino == 0) return perr;
    uint32_t child_ino;
    if (ext2_dir_lookup(parent_ino, basename, &child_ino) < 0) return -ENOENT;
    struct ext2_inode ip;
    if (ext2_inode_read(child_ino, &ip) < 0) return -EIO;
    if ((ip.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -ENOTDIR;

    /* Count entries (only . and .. allowed) */
    /* TODO: use ext2_dir_iterate to count */
    int rc = ext2_dir_remove(parent_ino, basename);
    if (rc == 0) {
        ext2_truncate(child_ino, 0);
        ext2_inode_free(child_ino);
        struct ext2_inode pip;
        if (ext2_inode_read(parent_ino, &pip) == 0 && pip.i_links_count > 0) {
            pip.i_links_count--;
            ext2_inode_write(parent_ino, &pip);
        }
    }
    return rc;
}

static int ext2_vfs_unlink(struct mount *mnt, const char *relpath) {
    (void)mnt;
    const char *basename;
    int perr = -ENOENT;
    uint64_t parent_ino64 = ext2_walk_parent_err(relpath, &basename, &perr);
    uint32_t parent_ino = (uint32_t)parent_ino64;
    if (parent_ino == 0) return perr;
    uint32_t child_ino;
    if (ext2_dir_lookup(parent_ino, basename, &child_ino) < 0) return -ENOENT;
    struct ext2_inode ip;
    if (ext2_inode_read(child_ino, &ip) < 0) return -EIO;
    if ((ip.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return -EISDIR;
    int rc = ext2_dir_remove(parent_ino, basename);
    if (rc == 0) {
        if (ip.i_links_count > 0) ip.i_links_count--;
        ext2_inode_write(child_ino, &ip);
        if (ip.i_links_count == 0 && ext2_open_count(child_ino) == 0) {
            ext2_truncate(child_ino, 0);
            ext2_inode_free(child_ino);
        }
        inotify_event(relpath, IN_DELETE);
    }
    return rc;
}

static int ext2_vfs_rename(struct mount *mnt, const char *oldrel, const char *newrel) {
    (void)mnt;
    const char *old_base;
    int oerr = -ENOENT;
    uint64_t old_parent64 = ext2_walk_parent_err(oldrel, &old_base, &oerr);
    uint32_t old_parent = (uint32_t)old_parent64;
    if (old_parent == 0) return oerr;

    const char *new_base;
    int nerr = -ENOENT;
    uint64_t new_parent64 = ext2_walk_parent_err(newrel, &new_base, &nerr);
    uint32_t new_parent = (uint32_t)new_parent64;
    if (new_parent == 0) return nerr;

    int rc = ext2_rename(old_parent, old_base, new_parent, new_base);
    if (rc == 0) {
        inotify_event(oldrel, IN_MOVED_FROM);
        inotify_event(newrel, IN_MOVED_TO);
    }
    return rc;
}

static int ext2_vfs_symlink(struct mount *mnt, const char *target, const char *relpath) {
    (void)mnt;
    uint64_t ino64 = ext2_walk_err(relpath, 0);
    if (ino64 != 0) return -EEXIST;

    const char *basename;
    int perr = -ENOENT;
    uint64_t parent_ino64 = ext2_walk_parent_err(relpath, &basename, &perr);
    uint32_t parent_ino = (uint32_t)parent_ino64;
    if (parent_ino == 0) return perr;

    struct ext2_inode pip;
    if (ext2_inode_read(parent_ino, &pip) < 0) return -EIO;
    if ((pip.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -ENOTDIR;

    return ext2_symlink_create(parent_ino, basename, target);
}

static int ext2_vfs_readlink(struct mount *mnt, const char *relpath,
                             char *buf, size_t bufsiz) {
    (void)mnt;
    const char *basename;
    int perr = -ENOENT;
    uint64_t parent_ino64 = ext2_walk_parent_err(relpath, &basename, &perr);
    uint32_t parent_ino = (uint32_t)parent_ino64;
    if (parent_ino == 0) return perr;
    uint32_t ino;
    if (ext2_dir_lookup(parent_ino, basename, &ino) < 0) return -ENOENT;
    return ext2_readlink(ino, buf, bufsiz);
}

static int ext2_vfs_chmod(struct mount *mnt, const char *relpath, uint32_t mode) {
    (void)mnt;
    int werr = -ENOENT;
    uint64_t ino64 = ext2_walk_err(relpath, &werr);
    uint32_t ino = (uint32_t)ino64;
    if (ino == 0) return werr;
    struct ext2_inode ip;
    if (ext2_inode_read(ino, &ip) < 0) return -EIO;
    ip.i_mode = (ip.i_mode & EXT2_S_IFMT) | (uint16_t)(mode & 07777);
    ext2_inode_write(ino, &ip);
    inotify_event(relpath, IN_ATTRIB);
    return 0;
}

static int ext2_vfs_chown(struct mount *mnt, const char *relpath,
                          uint32_t uid, uint32_t gid) {
    (void)mnt;
    int werr = -ENOENT;
    uint64_t ino64 = ext2_walk_err(relpath, &werr);
    uint32_t ino = (uint32_t)ino64;
    if (ino == 0) return werr;
    struct ext2_inode ip;
    if (ext2_inode_read(ino, &ip) < 0) return -EIO;
    if (uid != (uint32_t)-1) ip.i_uid = (uint16_t)uid;
    if (gid != (uint32_t)-1) ip.i_gid = (uint16_t)gid;
    ip.i_mode &= ~(uint16_t)(04000 | 02000);
    ext2_inode_write(ino, &ip);
    inotify_event(relpath, IN_ATTRIB);
    return 0;
}

static int ext2_vfs_truncate(struct mount *mnt, const char *relpath, int64_t length) {
    (void)mnt;
    if (length < 0) return -EINVAL;
    int werr = -ENOENT;
    uint64_t ino64 = ext2_walk_err(relpath, &werr);
    uint32_t ino = (uint32_t)ino64;
    if (ino == 0) return werr;
    struct ext2_inode ip;
    if (ext2_inode_read(ino, &ip) < 0) return -EIO;
    if ((ip.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return -EISDIR;
    return ext2_truncate(ino, (size_t)length);
}

static int ext2_vfs_utimensat(struct mount *mnt, const char *relpath,
                              const int64_t times[4], int flags) {
    (void)mnt; (void)flags;
    #define UTIME_NOW_  ((1L << 30) - 1L)
    #define UTIME_OMIT_ ((1L << 30) - 2L)

    int werr = -ENOENT;
    uint64_t ino64 = ext2_walk_err(relpath, &werr);
    uint32_t ino = (uint32_t)ino64;
    if (ino == 0) return werr;
    struct ext2_inode ip;
    if (ext2_inode_read(ino, &ip) < 0) return -EIO;
    if (times) {
        uint32_t now = timer_epoch_sec();
        int64_t atime_nsec = times[1], mtime_nsec = times[3];
        if (atime_nsec != UTIME_OMIT_)
            ip.i_atime = (atime_nsec == UTIME_NOW_) ? now : (uint32_t)times[0];
        if (mtime_nsec != UTIME_OMIT_)
            ip.i_mtime = (mtime_nsec == UTIME_NOW_) ? now : (uint32_t)times[2];
    } else {
        uint32_t now = timer_epoch_sec();
        ip.i_atime = now;
        ip.i_mtime = now;
    }
    ext2_inode_write(ino, &ip);
    return 0;
}

static int ext2_vfs_link(struct mount *mnt, const char *oldrel, const char *newrel) {
    (void)mnt;
    int oerr = -ENOENT;
    uint64_t old_ino64 = ext2_walk_err(oldrel, &oerr);
    uint32_t old_ino = (uint32_t)old_ino64;
    if (old_ino == 0) return oerr;

    const char *new_base;
    int nerr = -ENOENT;
    uint64_t new_parent64 = ext2_walk_parent_err(newrel, &new_base, &nerr);
    uint32_t new_parent = (uint32_t)new_parent64;
    if (new_parent == 0) return nerr;

    struct ext2_inode ip;
    if (ext2_inode_read(old_ino, &ip) < 0) return -EIO;
    if ((ip.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return -EPERM;

    uint8_t ft = EXT2_FT_REG_FILE;
    if ((ip.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK) ft = EXT2_FT_SYMLINK;

    int rc = ext2_dir_add(new_parent, new_base, old_ino, ft);
    if (rc < 0) return rc;
    ip.i_links_count++;
    ext2_inode_write(old_ino, &ip);
    return 0;
}

/* ── file_ops ───────────────────────────────────── */

static long ext2_vfs_read(struct vfs_file *f, void *buf, size_t count) {
    if (f->type != VFS_FILE) return -EISDIR;
    uint8_t kbuf[4096];
    size_t total = 0;
    while (total < count) {
        size_t chunk = count - total;
        if (chunk > 4096) chunk = 4096;
        int rc = ext2_read((uint32_t)f->disk_ino, kbuf,
                           (size_t)f->offset + total, chunk);
        if (rc < 0) return total > 0 ? (long)total : rc;
        if (rc == 0) break;
        kmemcpy((uint8_t *)buf + total, kbuf, (size_t)rc);
        total += (size_t)rc;
        if ((size_t)rc < chunk) break;
    }
    f->offset += (uint64_t)total;
    return (long)total;
}

static long ext2_vfs_write(struct vfs_file *f, const void *buf, size_t count) {
    if (f->type != VFS_FILE) return -EISDIR;
    if (f->flags & O_APPEND) {
        struct ext2_inode ip;
        if (ext2_inode_read((uint32_t)f->disk_ino, &ip) == 0)
            f->offset = ip.i_size;
    }
    uint8_t kbuf[4096];
    size_t total = 0;
    while (total < count) {
        size_t chunk = count - total;
        if (chunk > 4096) chunk = 4096;
        kmemcpy(kbuf, (const uint8_t *)buf + total, chunk);
        int rc = ext2_write((uint32_t)f->disk_ino, kbuf,
                            (size_t)f->offset + total, chunk);
        if (rc < 0) return total > 0 ? (long)total : rc;
        total += (size_t)rc;
        if ((size_t)rc < chunk) break;
    }
    f->offset += (uint64_t)total;
    f->disk_size = f->offset;
    page_cache_invalidate_ino(f->disk_ino);
    if (total > 0 && f->path[0])
        inotify_event(f->path, IN_MODIFY);
    return (long)total;
}

static long ext2_vfs_lseek(struct vfs_file *f, long offset, int whence) {
    uint64_t sz = f->disk_size;
    struct ext2_inode ip;
    if (ext2_inode_read((uint32_t)f->disk_ino, &ip) == 0)
        sz = ip.i_size;
    long new_off;
    switch (whence) {
    case SEEK_SET: new_off = offset; break;
    case SEEK_CUR: new_off = (long)f->offset + offset; break;
    case SEEK_END: new_off = (long)sz + offset; break;
    default: return -EINVAL;
    }
    if (new_off < 0) return -EINVAL;
    f->offset = (uint64_t)new_off;
    return new_off;
}

static long ext2_vfs_pread(struct vfs_file *f, void *buf, size_t count, uint64_t offset) {
    if (f->type != VFS_FILE) return -EISDIR;
    return (long)ext2_read((uint32_t)f->disk_ino, buf, (size_t)offset, count);
}

static long ext2_vfs_pwrite(struct vfs_file *f, const void *buf,
                            size_t count, uint64_t offset) {
    if (f->type != VFS_FILE) return -EISDIR;
    uint8_t kbuf[4096];
    size_t total = 0;
    while (total < count) {
        size_t chunk = count - total;
        if (chunk > 4096) chunk = 4096;
        kmemcpy(kbuf, (const uint8_t *)buf + total, chunk);
        int rc = ext2_write((uint32_t)f->disk_ino, kbuf,
                            (size_t)offset + total, chunk);
        if (rc < 0) return total > 0 ? (long)total : rc;
        total += (size_t)rc;
        if ((size_t)rc < chunk) break;
    }
    if (total > 0 && f->path[0])
        inotify_event(f->path, IN_MODIFY);
    return (long)total;
}

static int ext2_vfs_close(struct vfs_file *f) {
    if (f->disk_ino)
        ext2_open_dec((uint32_t)f->disk_ino);
    return 0;
}

static int ext2_vfs_fstat(struct vfs_file *f, struct k_stat *buf) {
    struct ext2_inode ip;
    if (ext2_inode_read((uint32_t)f->disk_ino, &ip) < 0) return -EIO;
    fill_ext2_stat((uint32_t)f->disk_ino, &ip, buf);
    return 0;
}

static int ext2_vfs_ftruncate(struct vfs_file *f, int64_t length) {
    if (length < 0) return -EINVAL;
    return ext2_truncate((uint32_t)f->disk_ino, (size_t)length);
}

static int ext2_vfs_fchmod(struct vfs_file *f, uint32_t mode) {
    struct ext2_inode ip;
    if (ext2_inode_read((uint32_t)f->disk_ino, &ip) < 0) return -EIO;
    ip.i_mode = (ip.i_mode & EXT2_S_IFMT) | (uint16_t)(mode & 07777);
    ext2_inode_write((uint32_t)f->disk_ino, &ip);
    return 0;
}

static int ext2_vfs_fchown(struct vfs_file *f, uint32_t uid, uint32_t gid) {
    struct ext2_inode ip;
    if (ext2_inode_read((uint32_t)f->disk_ino, &ip) < 0) return -EIO;
    if (uid != (uint32_t)-1) ip.i_uid = (uint16_t)uid;
    if (gid != (uint32_t)-1) ip.i_gid = (uint16_t)gid;
    ip.i_mode &= ~(uint16_t)(04000 | 02000);
    ext2_inode_write((uint32_t)f->disk_ino, &ip);
    return 0;
}

static int ext2_vfs_fsync(struct vfs_file *f) {
    (void)f;
    ext2_sync();
    return 0;
}

static void ext2_vfs_sync(struct mount *mnt) {
    (void)mnt;
    ext2_sync();
}

/* ── Exported ops tables ────────────────────────── */

struct inode_ops ext2_inode_ops = {
    .lookup    = ext2_vfs_lookup,
    .stat      = ext2_vfs_stat,
    .lstat     = ext2_vfs_lstat,
    .mkdir     = ext2_vfs_mkdir,
    .rmdir     = ext2_vfs_rmdir,
    .unlink    = ext2_vfs_unlink,
    .rename    = ext2_vfs_rename,
    .symlink   = ext2_vfs_symlink,
    .readlink  = ext2_vfs_readlink,
    .link      = ext2_vfs_link,
    .chmod     = ext2_vfs_chmod,
    .chown     = ext2_vfs_chown,
    .truncate  = ext2_vfs_truncate,
    .utimensat = ext2_vfs_utimensat,
};

struct file_ops ext2_file_ops = {
    .read      = ext2_vfs_read,
    .write     = ext2_vfs_write,
    .lseek     = ext2_vfs_lseek,
    .pread     = ext2_vfs_pread,
    .pwrite    = ext2_vfs_pwrite,
    .close     = ext2_vfs_close,
    .fstat     = ext2_vfs_fstat,
    .ftruncate = ext2_vfs_ftruncate,
    .fchmod    = ext2_vfs_fchmod,
    .fchown    = ext2_vfs_fchown,
    .fsync     = ext2_vfs_fsync,
};

struct super_ops ext2_super_ops = {
    .sync = ext2_vfs_sync,
};
