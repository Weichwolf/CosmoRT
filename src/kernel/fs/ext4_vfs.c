/* CosmoRT ext4 filesystem — VFS ops wrapper
 *
 * Wraps the per-instance ext4 API into VFS inode_ops / file_ops for
 * mount-table dispatch. mnt->fs_data is a struct ext4_fs *; legacy disk-mount
 * passes NULL → default instance. Loop-mounts pass their own instance.
 */

#include "fs/vfs.h"
#include "fs/ext4.h"
#include "fs/bcache.h"
#include "hw/serial.h"
#include "memops.h"
#include "proc/process.h"
#include "fs/vfs_internal.h"

extern void inotify_event(const char *path, uint32_t mask);
extern void page_cache_invalidate_ino(uint64_t ino);
extern uint32_t timer_epoch_sec(void);

#define NAME_MAX 255

/* Resolve struct ext4_fs from a mount. NULL → default instance. */
static inline struct ext4_fs *fs_of(struct mount *mnt) {
    if (!mnt || !mnt->fs_data) return ext4_default_fs();
    return (struct ext4_fs *)mnt->fs_data;
}

/* ── inode_ops ──────────────────────────────────── */

static int ext4_vfs_lookup(struct mount *mnt, const char *relpath, int follow,
                           uint64_t *handle, int *err) {
    (void)follow;
    struct ext4_fs *fs = fs_of(mnt);
    uint64_t ino = ext4_walk_inst_err(fs, relpath, err);
    if (ino == 0) return -1;
    *handle = ino;
    return 0;
}

static int ext4_vfs_stat(struct mount *mnt, const char *relpath, struct k_stat *buf) {
    struct ext4_fs *fs = fs_of(mnt);
    int err = -ENOENT;
    uint64_t ino64 = ext4_walk_inst_err(fs, relpath, &err);
    uint32_t ino = (uint32_t)ino64;
    if (ino == 0) return err;
    struct ext4_inode ip;
    if (ext4_inode_read_inst(fs, ino, &ip) < 0) return -EIO;
    fill_ext4_stat(ino, &ip, buf);
    return 0;
}

static int ext4_vfs_lstat(struct mount *mnt, const char *relpath, struct k_stat *buf) {
    struct ext4_fs *fs = fs_of(mnt);
    const char *basename;
    int perr = -ENOENT;
    uint64_t parent_ino64 = ext4_walk_parent_inst_err(fs, relpath, &basename, &perr);
    uint32_t parent_ino = (uint32_t)parent_ino64;
    if (parent_ino == 0) {
        if (relpath[0] == '/' && relpath[1] == 0) {
            struct ext4_inode ip;
            if (ext4_inode_read_inst(fs, EXT4_ROOT_INO, &ip) < 0) return -EIO;
            fill_ext4_stat(EXT4_ROOT_INO, &ip, buf);
            return 0;
        }
        return perr;
    }
    uint32_t ino;
    if (ext4_dir_lookup_inst(fs, parent_ino, basename, &ino) < 0) return -ENOENT;
    struct ext4_inode ip;
    if (ext4_inode_read_inst(fs, ino, &ip) < 0) return -EIO;
    fill_ext4_stat(ino, &ip, buf);
    return 0;
}

static int ext4_vfs_mkdir(struct mount *mnt, const char *relpath, int mode) {
    struct ext4_fs *fs = fs_of(mnt);
    uint64_t ino64 = ext4_walk_inst_err(fs, relpath, 0);
    if (ino64 != 0) return -EEXIST;

    const char *basename;
    int perr = -ENOENT;
    uint64_t parent_ino64 = ext4_walk_parent_inst_err(fs, relpath, &basename, &perr);
    uint32_t parent_ino = (uint32_t)parent_ino64;
    if (parent_ino == 0) return perr;

    struct ext4_inode pip;
    if (ext4_inode_read_inst(fs, parent_ino, &pip) < 0) return -EIO;
    process_t *pcur = proc_current();
    uint32_t cmask = pcur ? pcur->umask_val : 0022;
    uint32_t cmode = ((uint32_t)mode & 07777) & ~(cmask & 0777);
    if (pip.i_mode & S_ISGID) cmode |= S_ISGID;

    uint32_t new_ino;
    int rc = ext4_mkdir_inst(fs, parent_ino, basename, (uint16_t)cmode, &new_ino);
    if (rc < 0) return rc;
    if (pcur) {
        struct ext4_inode nip;
        if (ext4_inode_read_inst(fs, new_ino, &nip) == 0) {
            nip.i_uid = (uint16_t)pcur->fsuid;
            nip.i_gid = (pip.i_mode & S_ISGID)
                            ? (uint16_t)pip.i_gid
                            : (uint16_t)pcur->fsgid;
            ext4_inode_write_inst(fs, new_ino, &nip);
        }
    }
    return rc;
}

static int ext4_vfs_rmdir(struct mount *mnt, const char *relpath) {
    struct ext4_fs *fs = fs_of(mnt);
    const char *basename;
    int perr = -ENOENT;
    uint64_t parent_ino64 = ext4_walk_parent_inst_err(fs, relpath, &basename, &perr);
    uint32_t parent_ino = (uint32_t)parent_ino64;
    if (parent_ino == 0) return perr;
    uint32_t child_ino;
    if (ext4_dir_lookup_inst(fs, parent_ino, basename, &child_ino) < 0) return -ENOENT;
    struct ext4_inode ip;
    if (ext4_inode_read_inst(fs, child_ino, &ip) < 0) return -EIO;
    if ((ip.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return -ENOTDIR;

    int rc = ext4_dir_remove_inst(fs, parent_ino, basename);
    if (rc == 0) {
        ext4_truncate_inst(fs, child_ino, 0);
        ext4_inode_free_inst(fs, child_ino);
        struct ext4_inode pip;
        if (ext4_inode_read_inst(fs, parent_ino, &pip) == 0 && pip.i_links_count > 0) {
            pip.i_links_count--;
            ext4_inode_write_inst(fs, parent_ino, &pip);
        }
    }
    return rc;
}

static int ext4_vfs_unlink(struct mount *mnt, const char *relpath) {
    struct ext4_fs *fs = fs_of(mnt);
    const char *basename;
    int perr = -ENOENT;
    uint64_t parent_ino64 = ext4_walk_parent_inst_err(fs, relpath, &basename, &perr);
    uint32_t parent_ino = (uint32_t)parent_ino64;
    if (parent_ino == 0) return perr;
    uint32_t child_ino;
    if (ext4_dir_lookup_inst(fs, parent_ino, basename, &child_ino) < 0) return -ENOENT;
    struct ext4_inode ip;
    if (ext4_inode_read_inst(fs, child_ino, &ip) < 0) return -EIO;
    if ((ip.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) return -EISDIR;
    int rc = ext4_dir_remove_inst(fs, parent_ino, basename);
    if (rc == 0) {
        if (ip.i_links_count > 0) ip.i_links_count--;
        ext4_inode_write_inst(fs, child_ino, &ip);
        if (ip.i_links_count == 0 && ext4_open_count(child_ino) == 0) {
            ext4_truncate_inst(fs, child_ino, 0);
            ext4_inode_free_inst(fs, child_ino);
        }
        inotify_event(relpath, IN_DELETE);
    }
    return rc;
}

static int ext4_vfs_rename(struct mount *mnt, const char *oldrel, const char *newrel) {
    struct ext4_fs *fs = fs_of(mnt);
    const char *old_base;
    int oerr = -ENOENT;
    uint64_t old_parent64 = ext4_walk_parent_inst_err(fs, oldrel, &old_base, &oerr);
    uint32_t old_parent = (uint32_t)old_parent64;
    if (old_parent == 0) return oerr;

    const char *new_base;
    int nerr = -ENOENT;
    uint64_t new_parent64 = ext4_walk_parent_inst_err(fs, newrel, &new_base, &nerr);
    uint32_t new_parent = (uint32_t)new_parent64;
    if (new_parent == 0) return nerr;

    int rc = ext4_rename_inst(fs, old_parent, old_base, new_parent, new_base);
    if (rc == 0) {
        inotify_event(oldrel, IN_MOVED_FROM);
        inotify_event(newrel, IN_MOVED_TO);
    }
    return rc;
}

static int ext4_vfs_symlink(struct mount *mnt, const char *target, const char *relpath) {
    struct ext4_fs *fs = fs_of(mnt);
    uint64_t ino64 = ext4_walk_inst_err(fs, relpath, 0);
    if (ino64 != 0) return -EEXIST;

    const char *basename;
    int perr = -ENOENT;
    uint64_t parent_ino64 = ext4_walk_parent_inst_err(fs, relpath, &basename, &perr);
    uint32_t parent_ino = (uint32_t)parent_ino64;
    if (parent_ino == 0) return perr;

    struct ext4_inode pip;
    if (ext4_inode_read_inst(fs, parent_ino, &pip) < 0) return -EIO;
    if ((pip.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return -ENOTDIR;

    return ext4_symlink_create_inst(fs, parent_ino, basename, target);
}

static int ext4_vfs_readlink(struct mount *mnt, const char *relpath,
                             char *buf, size_t bufsiz) {
    struct ext4_fs *fs = fs_of(mnt);
    const char *basename;
    int perr = -ENOENT;
    uint64_t parent_ino64 = ext4_walk_parent_inst_err(fs, relpath, &basename, &perr);
    uint32_t parent_ino = (uint32_t)parent_ino64;
    if (parent_ino == 0) return perr;
    uint32_t ino;
    if (ext4_dir_lookup_inst(fs, parent_ino, basename, &ino) < 0) return -ENOENT;
    return ext4_readlink_inst(fs, ino, buf, bufsiz);
}

static int ext4_vfs_chmod(struct mount *mnt, const char *relpath, uint32_t mode) {
    struct ext4_fs *fs = fs_of(mnt);
    int werr = -ENOENT;
    uint64_t ino64 = ext4_walk_inst_err(fs, relpath, &werr);
    uint32_t ino = (uint32_t)ino64;
    if (ino == 0) return werr;
    struct ext4_inode ip;
    if (ext4_inode_read_inst(fs, ino, &ip) < 0) return -EIO;
    process_t *cur = proc_current();
    if (!cred_can_chmod(cur, ip.i_uid)) return -EPERM;
    uint32_t new_mode = mode & 07777;
    if (cur && cur->euid != 0 && (new_mode & S_ISGID) &&
        !cred_in_group(cur, ip.i_gid))
        new_mode &= ~(uint32_t)S_ISGID;
    ip.i_mode = (ip.i_mode & EXT4_S_IFMT) | (uint16_t)new_mode;
    ip.i_ctime = timer_epoch_sec();
    ext4_inode_write_inst(fs, ino, &ip);
    inotify_event(relpath, IN_ATTRIB);
    return 0;
}

static void ext4_drop_suid_sgid(struct ext4_inode *ip) {
    if ((ip->i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) return;
    if (ip->i_mode & S_ISUID)
        ip->i_mode &= (uint16_t)~S_ISUID;
    if ((ip->i_mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP))
        ip->i_mode &= (uint16_t)~S_ISGID;
}

static int ext4_chown_common(struct ext4_fs *fs, uint32_t ino, struct ext4_inode *ip,
                             uint32_t uid, uint32_t gid) {
    process_t *cur = proc_current();
    if (cur && cur->euid != 0) {
        if (uid != (uint32_t)-1 && uid != ip->i_uid) return -EPERM;
        if (!cred_owns(cur, ip->i_uid)) return -EPERM;
        if (gid != (uint32_t)-1 && !cred_in_group(cur, gid)) return -EPERM;
    }
    if (uid != (uint32_t)-1) ip->i_uid = (uint16_t)uid;
    if (gid != (uint32_t)-1) ip->i_gid = (uint16_t)gid;
    ext4_drop_suid_sgid(ip);
    ip->i_ctime = timer_epoch_sec();
    ext4_inode_write_inst(fs, ino, ip);
    return 0;
}

static int ext4_vfs_chown(struct mount *mnt, const char *relpath,
                          uint32_t uid, uint32_t gid) {
    struct ext4_fs *fs = fs_of(mnt);
    int werr = -ENOENT;
    uint64_t ino64 = ext4_walk_inst_err(fs, relpath, &werr);
    uint32_t ino = (uint32_t)ino64;
    if (ino == 0) return werr;
    struct ext4_inode ip;
    if (ext4_inode_read_inst(fs, ino, &ip) < 0) return -EIO;
    int rc = ext4_chown_common(fs, ino, &ip, uid, gid);
    if (rc == 0) inotify_event(relpath, IN_ATTRIB);
    return rc;
}

static int ext4_vfs_lchown(struct mount *mnt, const char *relpath,
                           uint32_t uid, uint32_t gid) {
    struct ext4_fs *fs = fs_of(mnt);
    const char *basename;
    int perr = -ENOENT;
    uint64_t parent_ino64 = ext4_walk_parent_inst_err(fs, relpath, &basename, &perr);
    uint32_t parent_ino = (uint32_t)parent_ino64;
    if (parent_ino == 0) return perr;
    uint32_t ino;
    if (ext4_dir_lookup_inst(fs, parent_ino, basename, &ino) < 0) return -ENOENT;
    struct ext4_inode ip;
    if (ext4_inode_read_inst(fs, ino, &ip) < 0) return -EIO;
    int rc = ext4_chown_common(fs, ino, &ip, uid, gid);
    if (rc == 0) inotify_event(relpath, IN_ATTRIB);
    return rc;
}

static int ext4_vfs_truncate(struct mount *mnt, const char *relpath, int64_t length) {
    struct ext4_fs *fs = fs_of(mnt);
    if (length < 0) return -EINVAL;
    int werr = -ENOENT;
    uint64_t ino64 = ext4_walk_inst_err(fs, relpath, &werr);
    uint32_t ino = (uint32_t)ino64;
    if (ino == 0) return werr;
    struct ext4_inode ip;
    if (ext4_inode_read_inst(fs, ino, &ip) < 0) return -EIO;
    if ((ip.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) return -EISDIR;
    return ext4_truncate_inst(fs, ino, (size_t)length);
}

static int ext4_vfs_utimensat(struct mount *mnt, const char *relpath,
                              const int64_t times[4], int flags) {
    (void)flags;
    struct ext4_fs *fs = fs_of(mnt);
    #define UTIME_NOW_  ((1L << 30) - 1L)
    #define UTIME_OMIT_ ((1L << 30) - 2L)

    int werr = -ENOENT;
    uint64_t ino64 = ext4_walk_inst_err(fs, relpath, &werr);
    uint32_t ino = (uint32_t)ino64;
    if (ino == 0) return werr;
    struct ext4_inode ip;
    if (ext4_inode_read_inst(fs, ino, &ip) < 0) return -EIO;

    struct ext4_inode_extra extra;
    int has_extra = (ext4_read_extra_inst(fs, ino, &extra) == 0);
    if (!has_extra) kmemset(&extra, 0, sizeof(extra));

    if (times) {
        uint64_t now64 = (uint64_t)timer_epoch_sec();
        int64_t atime_nsec = times[1], mtime_nsec = times[3];
        if (atime_nsec != UTIME_OMIT_) {
            uint64_t ts = (atime_nsec == UTIME_NOW_) ? now64 : (uint64_t)times[0];
            uint32_t base, ex;
            ext4_encode_ts(ts, &base, &ex);
            ip.i_atime = base; extra.i_atime_extra = ex;
        }
        if (mtime_nsec != UTIME_OMIT_) {
            uint64_t ts = (mtime_nsec == UTIME_NOW_) ? now64 : (uint64_t)times[2];
            uint32_t base, ex;
            ext4_encode_ts(ts, &base, &ex);
            ip.i_mtime = base; extra.i_mtime_extra = ex;
        }
    } else {
        uint64_t now64 = (uint64_t)timer_epoch_sec();
        uint32_t base, ex;
        ext4_encode_ts(now64, &base, &ex);
        ip.i_atime = base; extra.i_atime_extra = ex;
        ip.i_mtime = base; extra.i_mtime_extra = ex;
    }
    ext4_inode_write_inst(fs, ino, &ip);
    if (has_extra) {
        extra.i_extra_isize = 28;
        ext4_write_extra_inst(fs, ino, &extra);
    }
    return 0;
}

int vfs_futimensat_ext4(uint64_t ino64, const int64_t times[4]) {
    /* Default-instance: futimensat operates on a fd which currently always
     * targets the default fs. Loop-mount fds would carry a fs ref via
     * vfs_file::mnt, but the existing callers do not pass that yet. */
    struct ext4_fs *fs = ext4_default_fs();
    #define UTIME_NOW_FD_  ((1L << 30) - 1L)
    #define UTIME_OMIT_FD_ ((1L << 30) - 2L)
    uint32_t ino = (uint32_t)ino64;
    struct ext4_inode ip;
    if (ext4_inode_read_inst(fs, ino, &ip) < 0) return -EIO;
    struct ext4_inode_extra extra;
    int has_extra = (ext4_read_extra_inst(fs, ino, &extra) == 0);
    if (!has_extra) kmemset(&extra, 0, sizeof(extra));
    uint64_t now64 = (uint64_t)timer_epoch_sec();
    if (times) {
        int64_t atime_nsec = times[1], mtime_nsec = times[3];
        if (atime_nsec != UTIME_OMIT_FD_) {
            uint64_t ts = (atime_nsec == UTIME_NOW_FD_) ? now64 : (uint64_t)times[0];
            uint32_t base, ex;
            ext4_encode_ts(ts, &base, &ex);
            ip.i_atime = base; extra.i_atime_extra = ex;
        }
        if (mtime_nsec != UTIME_OMIT_FD_) {
            uint64_t ts = (mtime_nsec == UTIME_NOW_FD_) ? now64 : (uint64_t)times[2];
            uint32_t base, ex;
            ext4_encode_ts(ts, &base, &ex);
            ip.i_mtime = base; extra.i_mtime_extra = ex;
        }
    } else {
        uint32_t base, ex;
        ext4_encode_ts(now64, &base, &ex);
        ip.i_atime = base; extra.i_atime_extra = ex;
        ip.i_mtime = base; extra.i_mtime_extra = ex;
    }
    ext4_inode_write_inst(fs, ino, &ip);
    if (has_extra) {
        extra.i_extra_isize = 28;
        ext4_write_extra_inst(fs, ino, &extra);
    }
    return 0;
}

static int ext4_vfs_link(struct mount *mnt, const char *oldrel, const char *newrel) {
    struct ext4_fs *fs = fs_of(mnt);
    int oerr = -ENOENT;
    uint64_t old_ino64 = ext4_walk_inst_err(fs, oldrel, &oerr);
    uint32_t old_ino = (uint32_t)old_ino64;
    if (old_ino == 0) return oerr;

    const char *new_base;
    int nerr = -ENOENT;
    uint64_t new_parent64 = ext4_walk_parent_inst_err(fs, newrel, &new_base, &nerr);
    uint32_t new_parent = (uint32_t)new_parent64;
    if (new_parent == 0) return nerr;

    struct ext4_inode ip;
    if (ext4_inode_read_inst(fs, old_ino, &ip) < 0) return -EIO;
    if ((ip.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) return -EPERM;

    uint8_t ft = EXT4_FT_REG_FILE;
    if ((ip.i_mode & EXT4_S_IFMT) == EXT4_S_IFLNK) ft = EXT4_FT_SYMLINK;

    int rc = ext4_dir_add_inst(fs, new_parent, new_base, old_ino, ft);
    if (rc < 0) return rc;
    ip.i_links_count++;
    ext4_inode_write_inst(fs, old_ino, &ip);
    return 0;
}

/* ── file_ops ───────────────────────────────────── */

static inline struct ext4_fs *fs_of_file(struct vfs_file *f) {
    return f && f->mnt ? fs_of(f->mnt) : ext4_default_fs();
}

static long ext4_vfs_read(struct vfs_file *f, void *buf, size_t count) {
    if (f->type != VFS_FILE) return -EISDIR;
    struct ext4_fs *fs = fs_of_file(f);
    uint8_t kbuf[16384];
    size_t total = 0;
    while (total < count) {
        size_t chunk = count - total;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        int rc = ext4_read_inst(fs, (uint32_t)f->disk_ino, kbuf,
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

static long ext4_vfs_write(struct vfs_file *f, const void *buf, size_t count) {
    if (f->type != VFS_FILE) return -EISDIR;
    struct ext4_fs *fs = fs_of_file(f);
    if (f->flags & O_APPEND) {
        struct ext4_inode ip;
        if (ext4_inode_read_inst(fs, (uint32_t)f->disk_ino, &ip) == 0)
            f->offset = ip.i_size;
    }
    uint8_t kbuf[4096];
    size_t total = 0;
    while (total < count) {
        size_t chunk = count - total;
        if (chunk > 4096) chunk = 4096;
        kmemcpy(kbuf, (const uint8_t *)buf + total, chunk);
        int rc = ext4_write_inst(fs, (uint32_t)f->disk_ino, kbuf,
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

static long ext4_vfs_lseek(struct vfs_file *f, long offset, int whence) {
    struct ext4_fs *fs = fs_of_file(f);
    uint64_t sz = f->disk_size;
    struct ext4_inode ip;
    if (ext4_inode_read_inst(fs, (uint32_t)f->disk_ino, &ip) == 0)
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

static long ext4_vfs_pread(struct vfs_file *f, void *buf, size_t count, uint64_t offset) {
    if (f->type != VFS_FILE) return -EISDIR;
    struct ext4_fs *fs = fs_of_file(f);
    return (long)ext4_read_inst(fs, (uint32_t)f->disk_ino, buf, (size_t)offset, count);
}

static long ext4_vfs_pwrite(struct vfs_file *f, const void *buf,
                            size_t count, uint64_t offset) {
    if (f->type != VFS_FILE) return -EISDIR;
    struct ext4_fs *fs = fs_of_file(f);
    uint8_t kbuf[4096];
    size_t total = 0;
    while (total < count) {
        size_t chunk = count - total;
        if (chunk > 4096) chunk = 4096;
        kmemcpy(kbuf, (const uint8_t *)buf + total, chunk);
        int rc = ext4_write_inst(fs, (uint32_t)f->disk_ino, kbuf,
                                 (size_t)offset + total, chunk);
        if (rc < 0) return total > 0 ? (long)total : rc;
        total += (size_t)rc;
        if ((size_t)rc < chunk) break;
    }
    if (total > 0 && f->path[0])
        inotify_event(f->path, IN_MODIFY);
    return (long)total;
}

static int ext4_vfs_close(struct vfs_file *f) {
    if (f->disk_ino)
        ext4_open_dec((uint32_t)f->disk_ino);
    return 0;
}

static int ext4_vfs_fstat(struct vfs_file *f, struct k_stat *buf) {
    struct ext4_fs *fs = fs_of_file(f);
    struct ext4_inode ip;
    if (ext4_inode_read_inst(fs, (uint32_t)f->disk_ino, &ip) < 0) return -EIO;
    fill_ext4_stat((uint32_t)f->disk_ino, &ip, buf);
    return 0;
}

static int ext4_vfs_ftruncate(struct vfs_file *f, int64_t length) {
    if (length < 0) return -EINVAL;
    struct ext4_fs *fs = fs_of_file(f);
    return ext4_truncate_inst(fs, (uint32_t)f->disk_ino, (size_t)length);
}

static int ext4_vfs_fchmod(struct vfs_file *f, uint32_t mode) {
    struct ext4_fs *fs = fs_of_file(f);
    struct ext4_inode ip;
    if (ext4_inode_read_inst(fs, (uint32_t)f->disk_ino, &ip) < 0) return -EIO;
    process_t *cur = proc_current();
    if (!cred_can_chmod(cur, ip.i_uid)) return -EPERM;
    uint32_t new_mode = mode & 07777;
    if (cur && cur->euid != 0 && (new_mode & S_ISGID) &&
        !cred_in_group(cur, ip.i_gid))
        new_mode &= ~(uint32_t)S_ISGID;
    ip.i_mode = (ip.i_mode & EXT4_S_IFMT) | (uint16_t)new_mode;
    ip.i_ctime = timer_epoch_sec();
    ext4_inode_write_inst(fs, (uint32_t)f->disk_ino, &ip);
    return 0;
}

static int ext4_vfs_fchown(struct vfs_file *f, uint32_t uid, uint32_t gid) {
    struct ext4_fs *fs = fs_of_file(f);
    struct ext4_inode ip;
    if (ext4_inode_read_inst(fs, (uint32_t)f->disk_ino, &ip) < 0) return -EIO;
    return ext4_chown_common(fs, (uint32_t)f->disk_ino, &ip, uid, gid);
}

static int ext4_vfs_fsync(struct vfs_file *f) {
    struct ext4_fs *fs = fs_of_file(f);
    ext4_sync_inst(fs);
    return 0;
}

static void ext4_vfs_sync(struct mount *mnt) {
    struct ext4_fs *fs = fs_of(mnt);
    ext4_sync_inst(fs);
}

/* ── Exported ops tables ────────────────────────── */

struct inode_ops ext4_inode_ops = {
    .lookup    = ext4_vfs_lookup,
    .stat      = ext4_vfs_stat,
    .lstat     = ext4_vfs_lstat,
    .mkdir     = ext4_vfs_mkdir,
    .rmdir     = ext4_vfs_rmdir,
    .unlink    = ext4_vfs_unlink,
    .rename    = ext4_vfs_rename,
    .symlink   = ext4_vfs_symlink,
    .readlink  = ext4_vfs_readlink,
    .link      = ext4_vfs_link,
    .chmod     = ext4_vfs_chmod,
    .chown     = ext4_vfs_chown,
    .lchown    = ext4_vfs_lchown,
    .truncate  = ext4_vfs_truncate,
    .utimensat = ext4_vfs_utimensat,
};

struct file_ops ext4_file_ops = {
    .read      = ext4_vfs_read,
    .write     = ext4_vfs_write,
    .lseek     = ext4_vfs_lseek,
    .pread     = ext4_vfs_pread,
    .pwrite    = ext4_vfs_pwrite,
    .close     = ext4_vfs_close,
    .fstat     = ext4_vfs_fstat,
    .ftruncate = ext4_vfs_ftruncate,
    .fchmod    = ext4_vfs_fchmod,
    .fchown    = ext4_vfs_fchown,
    .fsync     = ext4_vfs_fsync,
};

struct super_ops ext4_super_ops = {
    .sync = ext4_vfs_sync,
};
