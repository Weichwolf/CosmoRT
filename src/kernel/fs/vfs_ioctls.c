/* CosmoRT VFS — stat, fstat, lstat, chmod, fchmod, fchown, truncate, ftruncate, utimensat */

#include "fs/vfs_internal.h"
#include "core/timer.h"

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

    buf->st_atime_sec = node->atime;
    buf->st_mtime_sec = node->mtime;
    buf->st_ctime_sec = node->ctime;
}

void fill_ext2_stat(uint32_t ino, struct ext2_inode *ip, struct k_stat *buf) {
    kmemset(buf, 0, sizeof(struct k_stat));
    buf->st_ino = ino;
    buf->st_dev = 1;  /* ext2 device */
    buf->st_nlink = ip->i_links_count;
    buf->st_size = (int64_t)ip->i_size;
    buf->st_blksize = 4096;
    buf->st_blocks = (int64_t)ip->i_blocks;
    buf->st_atime_sec = (int64_t)ip->i_atime;
    buf->st_mtime_sec = (int64_t)ip->i_mtime;
    buf->st_ctime_sec = (int64_t)ip->i_ctime;

    buf->st_uid = ip->i_uid;
    buf->st_gid = ip->i_gid;

    /* Mode already contains type + permissions in ext2 */
    buf->st_mode = ip->i_mode;
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
    /* /dev/console → char device major 5, minor 1 */
    if (kstreq(path, "/dev/console")) {
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = S_IFCHR | 0600;
        buf->st_rdev = 0x0501;
        buf->st_ino = 9;
        buf->st_blksize = 4096;
        return 0;
    }
    /* /dev/tty1-tty12 → char device major 4, minor N */
    if (path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v' &&
        path[4]=='/' && path[5]=='t' && path[6]=='t' && path[7]=='y' &&
        path[8] >= '1' && path[8] <= '9') {
        int vt_num = 0;
        const char *d = path + 8;
        while (*d >= '0' && *d <= '9') vt_num = vt_num * 10 + (*d++ - '0');
        if (*d == '\0' && vt_num >= 1 && vt_num <= 12) {
            if (vt_num - 1 >= PTY_MAX) return -ENOENT;
            kmemset(buf, 0, sizeof(struct k_stat));
            buf->st_mode = S_IFCHR | 0620;
            buf->st_rdev = (uint64_t)(0x0400 + vt_num); /* major 4, minor N */
            buf->st_ino = (uint64_t)(100 + vt_num);
            buf->st_blksize = 4096;
            return 0;
        }
    }
    /* /dev/pts/N → char device major 136, minor N */
    if (path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v' &&
        path[4]=='/' && path[5]=='p' && path[6]=='t' && path[7]=='s' &&
        path[8]=='/') {
        int pts_id = 0;
        const char *d = path + 9;
        if (*d >= '0' && *d <= '9') {
            while (*d >= '0' && *d <= '9') pts_id = pts_id * 10 + (*d++ - '0');
            if (*d == '\0' && pts_id >= 0 && pts_id < PTY_MAX) {
                kmemset(buf, 0, sizeof(struct k_stat));
                buf->st_mode = S_IFCHR | 0620;
                buf->st_rdev = (uint64_t)(0x8800 + pts_id); /* major 136, minor N */
                buf->st_ino = (uint64_t)(200 + pts_id);
                buf->st_blksize = 4096;
                return 0;
            }
        }
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
        uint64_t ino64 = ext2_walk(path);
        uint32_t ino = (uint32_t)ino64;
        if (ino == 0) return -ENOENT;
        struct ext2_inode ip;
        if (ext2_inode_read(ino, &ip) < 0) return -EIO;
        fill_ext2_stat(ino, &ip, buf);
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

    if (f->backend == VFS_BACKEND_EXT2) {
        struct ext2_inode ip;
        if (ext2_inode_read((uint32_t)f->disk_ino, &ip) < 0) return -EIO;
        fill_ext2_stat((uint32_t)f->disk_ino, &ip, buf);
        return 0;
    }

    if (!f->node) return -EBADF;
    fill_stat(f->node, buf);
    return 0;
}

/* ── Metadata operations ─────────────────────────── */

int vfs_chmod(const char *path, uint32_t mode) {
    if (!is_ramfs_path(path)) {
        uint64_t ino64 = ext2_walk(path);
        uint32_t ino = (uint32_t)ino64;
        if (ino == 0) return -ENOENT;
        struct ext2_inode ip;
        if (ext2_inode_read(ino, &ip) < 0) return -EIO;
        ip.i_mode = (ip.i_mode & EXT2_S_IFMT) | (uint16_t)(mode & 07777);
        ext2_inode_write(ino, &ip);
        inotify_event(path, IN_ATTRIB);
        return 0;
    }
    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    node->mode = mode & 07777;
    inotify_event(path, IN_ATTRIB);
    return 0;
}

int vfs_fchmod(int fd, uint32_t mode) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;

    if (f->backend == VFS_BACKEND_EXT2) {
        struct ext2_inode ip;
        if (ext2_inode_read((uint32_t)f->disk_ino, &ip) < 0) return -EIO;
        ip.i_mode = (ip.i_mode & EXT2_S_IFMT) | (uint16_t)(mode & 07777);
        ext2_inode_write((uint32_t)f->disk_ino, &ip);
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

    if (f->backend == VFS_BACKEND_EXT2) {
        struct ext2_inode ip;
        if (ext2_inode_read((uint32_t)f->disk_ino, &ip) < 0) return -EIO;
        if (uid != (uint32_t)-1) ip.i_uid = (uint16_t)uid;
        if (gid != (uint32_t)-1) ip.i_gid = (uint16_t)gid;
        ext2_inode_write((uint32_t)f->disk_ino, &ip);
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
        uint64_t ino64 = ext2_walk(path);
        uint32_t ino = (uint32_t)ino64;
        if (ino == 0) return -ENOENT;
        struct ext2_inode ip;
        if (ext2_inode_read(ino, &ip) < 0) return -EIO;
        if ((ip.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return -EISDIR;
        return ext2_truncate(ino, (size_t)length);
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

    if (f->backend == VFS_BACKEND_EXT2) {
        return ext2_truncate((uint32_t)f->disk_ino, (size_t)length);
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
    #define UTIME_NOW_  ((1L << 30) - 1L)
    #define UTIME_OMIT_ ((1L << 30) - 2L)

    if (!path) return -EINVAL;

    if (!is_ramfs_path(path)) {
        uint64_t ino64 = ext2_walk(path);
        uint32_t ino = (uint32_t)ino64;
        if (ino == 0) goto try_ramfs; /* ext2 miss → try ramfs (e.g. /dev nodes) */
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

try_ramfs:;
    /* ramfs: update timestamps on VFS node */
    int lookup_err = 0;
    extern struct vfs_node *vfs_lookup_err(const char *path, int *err);
    struct vfs_node *node = vfs_lookup_err(path, &lookup_err);
    if (!node) return lookup_err ? lookup_err : -ENOENT;
    if (times) {
        #define UTIME_NOW  ((1L << 30) - 1L)
        #define UTIME_OMIT ((1L << 30) - 2L)
        uint32_t now = timer_epoch_sec();
        int64_t atime_nsec = times[1], mtime_nsec = times[3];
        if (atime_nsec != UTIME_OMIT)
            node->atime = (atime_nsec == UTIME_NOW) ? now : (uint32_t)times[0];
        if (mtime_nsec != UTIME_OMIT)
            node->mtime = (mtime_nsec == UTIME_NOW) ? now : (uint32_t)times[2];
    } else {
        /* NULL times = set both to current time */
        uint32_t now = timer_epoch_sec();
        node->atime = now;
        node->mtime = now;
    }
    return 0;
}

/* futimens on ext2 file by inode number */
int vfs_futimensat_ext2(uint64_t ino, const int64_t times[4]) {
    struct ext2_inode ip;
    if (ext2_inode_read((uint32_t)ino, &ip) < 0) return -EIO;
    if (times) {
        uint32_t now = timer_epoch_sec();
        int64_t a_ns = times[1], m_ns = times[3];
        if (a_ns != UTIME_OMIT_)
            ip.i_atime = (a_ns == UTIME_NOW_) ? now : (uint32_t)times[0];
        if (m_ns != UTIME_OMIT_)
            ip.i_mtime = (m_ns == UTIME_NOW_) ? now : (uint32_t)times[2];
    } else {
        uint32_t now = timer_epoch_sec();
        ip.i_atime = now;
        ip.i_mtime = now;
    }
    ext2_inode_write((uint32_t)ino, &ip);
    return 0;
}
