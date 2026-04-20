/* CosmoRT VFS — stat, fstat, chmod, fchmod, fchown, truncate, ftruncate, utimensat
 *
 * Path-based ops dispatch via mount table (mnt->i_ops->...).
 * FD-based ops dispatch via f->f_ops->... .
 */

#include "fs/vfs_internal.h"
#include "core/timer.h"

/* ── Stat helpers (used by tmpfs + ext4_vfs) ────── */

void fill_stat(struct vfs_inode *inode, struct k_stat *buf) {
    kmemset(buf, 0, sizeof(struct k_stat));
    buf->st_ino = inode->ino;
    buf->st_nlink = inode->nlink ? inode->nlink : 1;
    buf->st_size = (int64_t)inode->size;
    buf->st_blksize = 4096;
    buf->st_blocks = (int64_t)((inode->size + 511) / 512);
    buf->st_uid = inode->uid;
    buf->st_gid = inode->gid;

    uint32_t type_bits = 0;
    if (inode->type == VFS_DIR)          type_bits = S_IFDIR;
    else if (inode->type == VFS_FILE)    type_bits = S_IFREG;
    else if (inode->type == VFS_PIPE)    type_bits = S_IFIFO;
    else if (inode->type == VFS_SYMLINK) type_bits = S_IFLNK;
    buf->st_mode = type_bits | (inode->mode & 07777);

    buf->st_atime_sec = (int64_t)inode->atime;
    buf->st_mtime_sec = (int64_t)inode->mtime;
    buf->st_ctime_sec = (int64_t)inode->ctime;
}

void fill_ext4_stat(uint32_t ino, struct ext4_inode *ip, struct k_stat *buf) {
    kmemset(buf, 0, sizeof(struct k_stat));
    buf->st_ino = ino;
    buf->st_dev = 1;
    buf->st_nlink = ip->i_links_count;
    buf->st_size = (int64_t)ip->i_size;
    buf->st_blksize = 4096;
    buf->st_blocks = (int64_t)ip->i_blocks;
    buf->st_uid = ip->i_uid;
    buf->st_gid = ip->i_gid;
    buf->st_mode = ip->i_mode;

    /* 64-bit timestamps: base + extra fields (if 256-byte inodes) */
    struct ext4_inode_extra extra;
    if (ext4_read_extra(ino, &extra) == 0) {
        buf->st_atime_sec = (int64_t)ext4_decode_ts(ip->i_atime, extra.i_atime_extra);
        buf->st_mtime_sec = (int64_t)ext4_decode_ts(ip->i_mtime, extra.i_mtime_extra);
        buf->st_ctime_sec = (int64_t)ext4_decode_ts(ip->i_ctime, extra.i_ctime_extra);
    } else {
        buf->st_atime_sec = (int64_t)ip->i_atime;
        buf->st_mtime_sec = (int64_t)ip->i_mtime;
        buf->st_ctime_sec = (int64_t)ip->i_ctime;
    }
}

/* Fallback: use stat to get the correct path error code when
 * a specific inode_op is not implemented for a mount. */
static int vfs_path_error(struct mount *mnt, const char *relpath) {
    if (mnt && mnt->i_ops && mnt->i_ops->stat) {
        struct k_stat st;
        int rc = mnt->i_ops->stat(mnt, relpath, &st);
        if (rc < 0) return rc;
    }
    return -ENOENT;
}

/* ── stat — dispatch via mount table ────────────── */

int vfs_stat(const char *path, struct k_stat *buf) {
    const char *relpath;
    struct mount *mnt = vfs_resolve_mount(path, &relpath);
    if (mnt && mnt->i_ops && mnt->i_ops->stat)
        return mnt->i_ops->stat(mnt, relpath, buf);
    return -ENOENT;
}

/* ── fstat — dispatch via f_ops or FD type ──────── */

int vfs_fstat(int fd, struct k_stat *buf) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;

    if (fde->type == FD_DEVICE) {
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
        int devid = (int)(uintptr_t)fde->obj;
        if (devid == 1) { buf->st_rdev = 0x0103; buf->st_ino = 5; }
        if (devid == 2) { buf->st_rdev = 0x0105; buf->st_ino = 6; }
        if (devid == 3) { buf->st_rdev = 0x0109; buf->st_ino = 7; }
        if (devid == 4) { buf->st_rdev = 0x0500; buf->st_ino = 8; }
        buf->st_blksize = 4096;
        return 0;
    }
    if (fde->type == FD_SERIAL) {
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = S_IFCHR | S_IRUSR | S_IWUSR;
        buf->st_rdev = 0x0501;
        buf->st_blksize = 4096;
        return 0;
    }
    if (fde->type == FD_PROCFS) {
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = S_IFREG | S_IRUSR;
        buf->st_ino = 0xFFFF0001;
        buf->st_blksize = 4096;
        return 0;
    }
    if (fde->type == FD_PIPE) {
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = S_IFIFO | S_IRUSR | S_IWUSR;
        buf->st_blksize = 4096;
        return 0;
    }
    if (fde->type == FD_SOCKET) {
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = S_IFSOCK | S_IRUSR | S_IWUSR;
        buf->st_blksize = 4096;
        return 0;
    }
    if (fde->type == FD_PTY_SLAVE || fde->type == FD_PTY_MASTER) {
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
        buf->st_rdev = 0x8800 + (uint64_t)(uintptr_t)fde->obj;
        buf->st_blksize = 4096;
        return 0;
    }
    if (fde->type == FD_EPOLL || fde->type == FD_EVENTFD ||
        fde->type == FD_TIMERFD || fde->type == FD_INOTIFY) {
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = S_IFREG | S_IRUSR | S_IWUSR;
        buf->st_blksize = 4096;
        return 0;
    }

    if (fde->type != FD_FILE) return -EBADF;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;
    if (f->f_ops && f->f_ops->fstat)
        return f->f_ops->fstat(f, buf);
    return -EBADF;
}

/* ── chmod/chown — dispatch via mount table ─────── */

int vfs_chmod(const char *path, uint32_t mode) {
    const char *relpath;
    struct mount *mnt = vfs_resolve_mount(path, &relpath);
    if (mnt && mnt->i_ops && mnt->i_ops->chmod)
        return mnt->i_ops->chmod(mnt, relpath, mode);
    return vfs_path_error(mnt, relpath);
}

int vfs_chown(const char *path, uint32_t uid, uint32_t gid) {
    const char *relpath;
    struct mount *mnt = vfs_resolve_mount(path, &relpath);
    if (mnt && mnt->i_ops && mnt->i_ops->chown)
        return mnt->i_ops->chown(mnt, relpath, uid, gid);
    return vfs_path_error(mnt, relpath);
}

int vfs_lchown(const char *path, uint32_t uid, uint32_t gid) {
    const char *relpath;
    struct mount *mnt = vfs_resolve_mount(path, &relpath);
    if (mnt && mnt->i_ops && mnt->i_ops->lchown)
        return mnt->i_ops->lchown(mnt, relpath, uid, gid);
    /* fallback: if no lchown, use chown */
    if (mnt && mnt->i_ops && mnt->i_ops->chown)
        return mnt->i_ops->chown(mnt, relpath, uid, gid);
    return -ENOENT;
}

/* ── fchmod/fchown — dispatch via f_ops ─────────── */

int vfs_fchmod(int fd, uint32_t mode) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;
    if (f->f_ops && f->f_ops->fchmod)
        return f->f_ops->fchmod(f, mode);
    return -EBADF;
}

int vfs_fchown(int fd, uint32_t uid, uint32_t gid) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;
    if (f->f_ops && f->f_ops->fchown)
        return f->f_ops->fchown(f, uid, gid);
    return -EBADF;
}

/* ── truncate — dispatch via mount table / f_ops ── */

int vfs_truncate(const char *path, int64_t length) {
    if (length < 0) return -EINVAL;
    const char *relpath;
    struct mount *mnt = vfs_resolve_mount(path, &relpath);
    if (mnt && mnt->i_ops && mnt->i_ops->truncate)
        return mnt->i_ops->truncate(mnt, relpath, length);
    return vfs_path_error(mnt, relpath);
}

int vfs_ftruncate(int fd, int64_t length) {
    if (length < 0) return -EINVAL;
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f || f->type != VFS_FILE) return -EINVAL;
    if (f->f_ops && f->f_ops->ftruncate)
        return f->f_ops->ftruncate(f, length);
    return -EBADF;
}

/* ── utimensat — dispatch via mount table ───────── */

int vfs_utimensat(const char *path, const int64_t times[4], int flags) {
    if (!path) return -EINVAL;
    const char *relpath;
    struct mount *mnt = vfs_resolve_mount(path, &relpath);
    if (mnt && mnt->i_ops && mnt->i_ops->utimensat)
        return mnt->i_ops->utimensat(mnt, relpath, times, flags);
    return vfs_path_error(mnt, relpath);
}
