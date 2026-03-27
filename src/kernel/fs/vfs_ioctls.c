/* CosmoRT VFS — stat, fstat, lstat, chmod, fchmod, fchown, truncate, ftruncate, utimensat */

#include "fs/vfs_internal.h"

/* ── Stat helpers ────────────────────────────────── */

void fill_stat(struct vfs_node *node, struct k_stat *buf) {
    kmemset(buf, 0, sizeof(struct k_stat));
    buf->st_ino = node->ino;
    buf->st_nlink = 1;
    buf->st_size = (int64_t)node->size;
    buf->st_blksize = 4096;
    buf->st_blocks = (int64_t)((node->size + 511) / 512);
    buf->st_uid = node->uid;
    buf->st_gid = node->gid;

    if (node->type == VFS_DIR)
        buf->st_mode = S_IFDIR | (node->mode ? node->mode : S_IRWXU);
    else if (node->type == VFS_FILE)
        buf->st_mode = S_IFREG | (node->mode ? node->mode : (S_IRUSR | S_IWUSR));
    else if (node->type == VFS_PIPE)
        buf->st_mode = S_IFIFO | (S_IRUSR | S_IWUSR);
    else if (node->type == VFS_SYMLINK)
        buf->st_mode = S_IFLNK | 0777;
}

void fill_cosmofs_stat(uint64_t ino, struct cosmofs_inode *ip, struct k_stat *buf) {
    kmemset(buf, 0, sizeof(struct k_stat));
    buf->st_ino = ino;
    buf->st_dev = 1;  /* CosmoFS device */
    buf->st_nlink = 1;
    buf->st_size = (int64_t)ip->size;
    buf->st_blksize = 4096;
    buf->st_blocks = (int64_t)((ip->size + 511) / 512);
    buf->st_atime_sec = (int64_t)(ip->atime / 1000000000ULL);
    buf->st_atime_nsec = (int64_t)(ip->atime % 1000000000ULL);
    buf->st_mtime_sec = (int64_t)(ip->mtime / 1000000000ULL);
    buf->st_mtime_nsec = (int64_t)(ip->mtime % 1000000000ULL);
    buf->st_ctime_sec = (int64_t)(ip->ctime / 1000000000ULL);
    buf->st_ctime_nsec = (int64_t)(ip->ctime % 1000000000ULL);

    buf->st_uid = ip->uid;
    buf->st_gid = ip->gid;

    uint32_t perms = ip->flags ? ip->flags : 0755;
    if (ip->type == COSMOFS_TYPE_DIR)
        buf->st_mode = S_IFDIR | perms;
    else if (ip->type == COSMOFS_TYPE_SYMLINK)
        buf->st_mode = S_IFLNK | 0777;
    else
        buf->st_mode = S_IFREG | perms;
}

/* ── Stat operations ─────────────────────────────── */

int vfs_stat(const char *path, struct k_stat *buf) {
    /* Device files */
    if (kstreq(path, "/dev/null") || kstreq(path, "/dev/zero") ||
        kstreq(path, "/dev/urandom") || kstreq(path, "/dev/random") ||
        kstreq(path, "/dev/tty")) {
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = S_IFCHR | 0666;
        buf->st_blksize = 4096;
        buf->st_ino = 5;
        if (kstreq(path, "/dev/null"))    buf->st_rdev = 0x0103;
        if (kstreq(path, "/dev/zero"))    buf->st_rdev = 0x0105;
        if (kstreq(path, "/dev/urandom") || kstreq(path, "/dev/random"))
                                           buf->st_rdev = 0x0109;
        if (kstreq(path, "/dev/tty"))     buf->st_rdev = 0x0500;
        return 0;
    }

    const char *pn = procfs_name(path);
    if (pn) {
        /* /proc root directory */
        if (*pn == '\0') {
            kmemset(buf, 0, sizeof(struct k_stat));
            buf->st_mode = S_IFDIR | 0555;
            buf->st_ino = 0xFFFF0000;
            buf->st_nlink = 2;
            buf->st_blksize = 4096;
            return 0;
        }
        int dummy;
        int pid_type = procfs_pid_exists(pn);
        if (procfs_stat(pn, &dummy) < 0 && !pid_type) return -ENOENT;
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = (pid_type == 2) ? (S_IFLNK | 0777) : (S_IFREG | S_IRUSR);
        buf->st_ino = 0xFFFF0001;  /* synthetic inode */
        buf->st_blksize = 4096;
        return 0;
    }
    if (!is_ramfs_path(path)) {
        uint64_t ino = cosmofs_walk(path);
        if (ino == 0) return -ENOENT;
        struct cosmofs_inode *ip = cosmofs_inode_read(ino);
        if (!ip) return -EIO;
        fill_cosmofs_stat(ino, ip, buf);
        return 0;
    }
    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    fill_stat(node, buf);
    return 0;
}

int vfs_fstat(int fd, struct k_stat *buf) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;

    /* Device FDs: /dev/null, /dev/zero, /dev/urandom, /dev/tty */
    if (fde->type == FD_DEVICE) {
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
        int devid = (int)(uintptr_t)fde->obj;
        if (devid == 1 /*DEV_NULL*/)    { buf->st_rdev = 0x0103; buf->st_ino = 5; }
        if (devid == 2 /*DEV_ZERO*/)    { buf->st_rdev = 0x0105; buf->st_ino = 6; }
        if (devid == 3 /*DEV_URANDOM*/) { buf->st_rdev = 0x0109; buf->st_ino = 7; }
        if (devid == 4 /*DEV_TTY*/)     { buf->st_rdev = 0x0500; buf->st_ino = 8; }
        buf->st_blksize = 4096;
        return 0;
    }

    /* Serial FDs: character device (isatty() must return true) */
    if (fde->type == FD_SERIAL) {
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = S_IFCHR | S_IRUSR | S_IWUSR;
        buf->st_rdev = 0x0501; /* /dev/console: major 5, minor 1 */
        buf->st_blksize = 4096;
        return 0;
    }

    /* procfs FDs */
    if (fde->type == FD_PROCFS) {
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = S_IFREG | S_IRUSR;
        buf->st_ino = 0xFFFF0001;
        buf->st_blksize = 4096;
        return 0;
    }

    /* Pipe FDs: anonymous pipe, report as FIFO */
    if (fde->type == FD_PIPE) {
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = S_IFIFO | S_IRUSR | S_IWUSR;
        buf->st_blksize = 4096;
        return 0;
    }

    /* Socket FDs */
    if (fde->type == FD_SOCKET) {
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = S_IFSOCK | S_IRUSR | S_IWUSR;
        buf->st_blksize = 4096;
        return 0;
    }

    /* PTY slave/master: character device */
    if (fde->type == FD_PTY_SLAVE || fde->type == FD_PTY_MASTER) {
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
        buf->st_rdev = 0x8800 + (uint64_t)(uintptr_t)fde->obj; /* pts/N */
        buf->st_blksize = 4096;
        return 0;
    }

    /* epoll / eventfd / timerfd / inotify: anonymous FDs */
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

    if (f->backend == VFS_BACKEND_COSMOFS) {
        struct cosmofs_inode *ip = cosmofs_inode_read(f->cosmofs_ino);
        if (!ip) return -EIO;
        fill_cosmofs_stat(f->cosmofs_ino, ip, buf);
        return 0;
    }

    if (!f->node) return -EBADF;
    fill_stat(f->node, buf);
    return 0;
}

/* ── Metadata operations ─────────────────────────── */

int vfs_chmod(const char *path, uint32_t mode) {
    if (!is_ramfs_path(path)) {
        uint64_t ino = cosmofs_walk(path);
        if (ino == 0) return -ENOENT;
        struct cosmofs_inode *ip = cosmofs_inode_read(ino);
        if (!ip) return -EIO;
        struct cosmofs_inode copy;
        kmemcpy(&copy, ip, sizeof(copy));
        copy.flags = (uint16_t)(mode & 07777);
        cosmofs_inode_write(ino, &copy);
        return 0;
    }
    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    node->mode = mode & 07777;
    return 0;
}

int vfs_fchmod(int fd, uint32_t mode) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;

    if (f->backend == VFS_BACKEND_COSMOFS) {
        struct cosmofs_inode *ip = cosmofs_inode_read(f->cosmofs_ino);
        if (!ip) return -EIO;
        struct cosmofs_inode copy;
        kmemcpy(&copy, ip, sizeof(copy));
        copy.flags = (uint16_t)(mode & 07777);
        cosmofs_inode_write(f->cosmofs_ino, &copy);
        return 0;
    }
    if (f->node) f->node->mode = mode & 07777;
    return 0;
}

int vfs_fchown(int fd, uint32_t uid, uint32_t gid) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;

    if (f->backend == VFS_BACKEND_COSMOFS) {
        struct cosmofs_inode *ip = cosmofs_inode_read(f->cosmofs_ino);
        if (!ip) return -EIO;
        struct cosmofs_inode copy;
        kmemcpy(&copy, ip, sizeof(copy));
        if (uid != (uint32_t)-1) copy.uid = uid;
        if (gid != (uint32_t)-1) copy.gid = gid;
        cosmofs_inode_write(f->cosmofs_ino, &copy);
        return 0;
    }
    if (f->node) {
        if (uid != (uint32_t)-1) f->node->uid = uid;
        if (gid != (uint32_t)-1) f->node->gid = gid;
    }
    return 0;
}

int vfs_truncate(const char *path, int64_t length) {
    if (length < 0) return -EINVAL;

    if (!is_ramfs_path(path)) {
        uint64_t ino = cosmofs_walk(path);
        if (ino == 0) return -ENOENT;
        struct cosmofs_inode *ip = cosmofs_inode_read(ino);
        if (!ip) return -EIO;
        if (ip->type == COSMOFS_TYPE_DIR) return -EISDIR;
        return cosmofs_truncate(ino, (size_t)length);
    }

    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    if (node->type == VFS_DIR) return -EISDIR;

    size_t new_size = (size_t)length;
    if (new_size > node->size) {
        if (grow_file(node, new_size) < 0) return -ENOMEM;
        kmemset(node->data + node->size, 0, new_size - node->size);
    }
    node->size = new_size;
    return 0;
}

int vfs_ftruncate(int fd, int64_t length) {
    if (length < 0) return -EINVAL;
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f || f->type != VFS_FILE) return -EINVAL;

    if (f->backend == VFS_BACKEND_COSMOFS) {
        return cosmofs_truncate(f->cosmofs_ino, (size_t)length);
    }

    if (!f->node) return -EBADF;
    struct vfs_node *node = f->node;
    size_t new_size = (size_t)length;
    if (new_size > node->size) {
        if (grow_file(node, new_size) < 0) return -ENOMEM;
        kmemset(node->data + node->size, 0, new_size - node->size);
    }
    node->size = new_size;
    return 0;
}

int vfs_utimensat(const char *path, const int64_t times[4], int flags) {
    (void)flags;
    /* times[0]=atime_sec, times[1]=atime_nsec, times[2]=mtime_sec, times[3]=mtime_nsec */
    /* path==NULL means futimens (already handled by caller) */

    if (!path) return -EINVAL;

    if (!is_ramfs_path(path)) {
        uint64_t ino = cosmofs_walk(path);
        if (ino == 0) return -ENOENT;
        struct cosmofs_inode *ip = cosmofs_inode_read(ino);
        if (!ip) return -EIO;
        struct cosmofs_inode copy;
        kmemcpy(&copy, ip, sizeof(copy));
        if (times) {
            copy.atime = (uint64_t)times[0] * 1000000000ULL + (uint64_t)times[1];
            copy.mtime = (uint64_t)times[2] * 1000000000ULL + (uint64_t)times[3];
        }
        cosmofs_inode_write(ino, &copy);
        return 0;
    }

    /* ramfs: no timestamps stored, no-op */
    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    return 0;
}
