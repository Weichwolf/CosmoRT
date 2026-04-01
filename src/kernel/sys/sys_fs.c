/* CosmoRT Syscall Layer — filesystem metadata/mutation syscalls */

#include "internal.h"

/* ── SYS_fstat (5) / SYS_stat (4) ───────────────── */

long do_fstat(int fd, struct k_stat *buf) {
    if (!user_ok((uint64_t)buf, sizeof(struct k_stat))) return -EFAULT;
    return vfs_fstat(fd, buf);
}

/* ── SYS_fstatat (262) — primary; stat/lstat delegate here via dispatch ── */

long do_fstatat(int dirfd, const char *path, struct k_stat *buf, int flags) {
    char kpath[PATH_MAX], rpath[PATH_MAX];
    int len = resolve_at_path(dirfd, path, kpath, PATH_MAX);
    if (len < 0) return len;
    if (!user_ok((uint64_t)buf, sizeof(struct k_stat))) return -EFAULT;
    resolve_path(kpath, rpath, PATH_MAX);
    if (flags & AT_SYMLINK_NOFOLLOW)
        return vfs_lstat(rpath, buf);
    return vfs_stat(rpath, buf);
}

/* ── SYS_mkdirat (258) — primary; mkdir delegates here via dispatch ── */

long do_mkdirat(int dirfd, const char *path, int mode) {
    (void)mode;
    char kpath[PATH_MAX];
    int len = resolve_at_path(dirfd, path, kpath, PATH_MAX);
    if (len < 0) return len;
    return vfs_mkdir(kpath);
}

/* ── SYS_unlinkat (263) — primary; unlink/rmdir delegate here via dispatch ── */

long do_unlinkat(int dirfd, const char *path, int flags) {
    char kpath[PATH_MAX];
    int len = resolve_at_path(dirfd, path, kpath, PATH_MAX);
    if (len < 0) return len;
    if (flags & AT_REMOVEDIR)
        return vfs_rmdir(kpath);
    return vfs_unlink(kpath);
}

/* ── SYS_renameat2 (316) — primary; rename delegates here via dispatch ── */

long do_renameat2(int olddirfd, const char *oldpath,
                          int newdirfd, const char *newpath, int flags) {
    /* Reject unsupported flag combinations */
    if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE)) return -EINVAL;
    if ((flags & RENAME_NOREPLACE) && (flags & RENAME_EXCHANGE)) return -EINVAL;
    if (flags & RENAME_EXCHANGE) return -EINVAL; /* not implemented */

    char kold[PATH_MAX], knew[PATH_MAX];
    int r = resolve_at_path(olddirfd, oldpath, kold, PATH_MAX);
    if (r < 0) return r;
    r = resolve_at_path(newdirfd, newpath, knew, PATH_MAX);
    if (r < 0) return r;

    if (flags & RENAME_NOREPLACE) {
        /* Target must not exist */
        extern uint64_t ext2_walk_path(const char *);
        if (vfs_lookup(knew) || ext2_walk_path(knew))
            return -EEXIST;
    }
    return vfs_rename(kold, knew);
}

/* ── SYS_fchmod (91) ─────────────────────────────── */

long do_fchmod(int fd, uint32_t mode) {
    return vfs_fchmod(fd, mode);
}

/* ── SYS_fchown (93) ─────────────────────────────── */

long do_fchown(int fd, uint32_t uid, uint32_t gid) {
    return vfs_fchown(fd, uid, gid);
}

/* ── SYS_linkat (265) — primary; link delegates here via dispatch ── */

long do_linkat(int olddirfd, const char *oldpath,
               int newdirfd, const char *newpath, int flags) {
    if (flags & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH)) return -EINVAL;
    if (flags & AT_EMPTY_PATH) return -ENOSYS; /* requires /proc/self/fd */

    char kold[PATH_MAX], knew[PATH_MAX];
    int r = resolve_at_path(olddirfd, oldpath, kold, PATH_MAX);
    if (r < 0) return r;
    r = resolve_at_path(newdirfd, newpath, knew, PATH_MAX);
    if (r < 0) return r;
    /* AT_SYMLINK_FOLLOW: resolve symlinks on source (default: don't follow).
     * vfs_link already operates on the resolved node, so the flag is a no-op
     * for our current symlink implementation (ramfs resolves on lookup). */
    return vfs_link(kold, knew);
}

/* ── SYS_symlinkat (266) — primary; symlink delegates here via dispatch ── */

long do_symlinkat(const char *target, int newdirfd, const char *linkpath) {
    char ktarget[PATH_MAX], klink[PATH_MAX];
    int r = copy_path_from_user(ktarget, target, PATH_MAX);
    if (r < 0) return r;
    r = resolve_at_path(newdirfd, linkpath, klink, PATH_MAX);
    if (r < 0) return r;
    return vfs_symlink(ktarget, klink);
}

/* ── SYS_readlinkat (267) — primary; readlink delegates here via dispatch ── */

long do_readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz) {
    char kpath[PATH_MAX];
    int r = resolve_at_path(dirfd, path, kpath, PATH_MAX);
    if (r < 0) return r;
    if (!user_ok((uint64_t)buf, bufsiz)) return -EFAULT;
    return vfs_readlink(kpath, buf, bufsiz);
}

/* ── SYS_truncate (76) / SYS_ftruncate (77) ─────── */

long do_truncate(const char *path, int64_t length) {
    char kpath[PATH_MAX], rpath[PATH_MAX];
    int r = copy_path_from_user(kpath, path, PATH_MAX);
    if (r < 0) return r;
    resolve_path(kpath, rpath, PATH_MAX);
    return vfs_truncate(rpath, length);
}

long do_ftruncate(int fd, int64_t length) {
    return vfs_ftruncate(fd, length);
}

/* ── SYS_fchmodat (268) ─────────────────────────── */

long do_fchmodat(int dirfd, const char *path, uint32_t mode, int flags) {
    (void)flags;
    char kpath[PATH_MAX];
    int r = resolve_at_path(dirfd, path, kpath, PATH_MAX);
    if (r < 0) return r;
    return vfs_chmod(kpath, mode);
}

/* ── SYS_utimensat (280) ────────────────────────── */

long do_utimensat(int dirfd, const char *path, const void *utimes, int flags) {
    int64_t ktimes[4];
    if (utimes) {
        int r2 = copy_from_user(ktimes, utimes, 2 * sizeof(struct k_timespec));
        if (r2) return r2;
    }

    /* futimens(fd, times): path==NULL, dirfd is the target fd */
    if (!path) {
        process_t *p = proc_current();
        if (!p) return -EFAULT;
        fd_entry_t *fde = fd_get(&p->fds, dirfd);
        if (!fde || fde->type == FD_NONE) return -EBADF;
        /* Get VFS file from fd */
        #define UTIME_NOW_FD  ((1L << 30) - 1L)
        #define UTIME_OMIT_FD ((1L << 30) - 2L)
        extern uint32_t timer_epoch_sec(void);
        if (fde->type == FD_FILE && fde->obj) {
            struct vfs_file *vf = (struct vfs_file *)fde->obj;
            if (vf->node) {
                /* ramfs file */
                struct vfs_node *node = vf->node;
                if (utimes) {
                    uint32_t now = timer_epoch_sec();
                    int64_t a_ns = ktimes[1], m_ns = ktimes[3];
                    if (a_ns != UTIME_OMIT_FD)
                        node->atime = (a_ns == UTIME_NOW_FD) ? now : (uint32_t)ktimes[0];
                    if (m_ns != UTIME_OMIT_FD)
                        node->mtime = (m_ns == UTIME_NOW_FD) ? now : (uint32_t)ktimes[2];
                } else {
                    uint32_t now = timer_epoch_sec();
                    node->atime = now;
                    node->mtime = now;
                }
                return 0;
            }
            if (vf->disk_ino) {
                /* ext2 file */
                extern int vfs_futimensat_ext2(uint64_t ino, const int64_t times[4]);
                return vfs_futimensat_ext2(vf->disk_ino, utimes ? ktimes : 0);
            }
        }
        return 0; /* no-op for special fds (pipes, sockets) */
    }

    char kpath[PATH_MAX];
    int r = resolve_at_path(dirfd, path, kpath, PATH_MAX);
    if (r < 0) return r;

    return vfs_utimensat(kpath, utimes ? ktimes : 0, flags);
}

/* ── SYS_fallocate (285) ────────────────────────── */

long do_fallocate(int fd, int mode, int64_t offset, int64_t len) {
    if (mode != 0) return -EOPNOTSUPP;
    if (offset < 0 || len <= 0) return -EINVAL;
    int64_t end = offset + len;
    /* Only extend, never shrink — check current size via fstat */
    struct k_stat st;
    int rc = vfs_fstat(fd, &st);
    if (rc < 0) return rc;
    if (end <= st.st_size) return 0;
    return vfs_ftruncate(fd, end);
}

/* ── SYS_mknodat (259) ──────────────────────────── */

long do_mknodat(int dirfd, const char *path, uint32_t mode, uint64_t dev) {
    (void)dev;

    /* Only S_IFREG (regular files) supported */
    if ((mode & S_IFMT) != S_IFREG && (mode & S_IFMT) != 0)
        return -EPERM;

    char kpath[PATH_MAX];
    int r = resolve_at_path(dirfd, path, kpath, PATH_MAX);
    if (r < 0) return r;

    /* Create as regular file via open+close */
    int fd = vfs_open(kpath, O_CREAT | O_WRONLY, (int)mode);
    if (fd < 0) return fd;
    return vfs_close(fd);
}

/* ── SYS_faccessat (269) — primary; access delegates here via dispatch ── */

long do_faccessat(int dirfd, const char *path, int mode, int flags) {
    (void)flags; /* AT_EACCESS / AT_SYMLINK_NOFOLLOW — irrelevant for single-user */
    if (mode & ~(F_OK | R_OK | W_OK | X_OK)) return -EINVAL;

    char kpath[PATH_MAX];
    int len = resolve_at_path(dirfd, path, kpath, PATH_MAX);
    if (len < 0) return len;

    /* Check existence across all filesystems */
    int exists = 0;
    int vfs_err = 0;

    /* ramfs */
    if (vfs_lookup_err(kpath, &vfs_err)) { exists = 1; goto found; }
    /* procfs entries (/proc/NAME) */
    {
        const char *pname = 0;
        if (kpath[0]=='/' && kpath[1]=='p' && kpath[2]=='r' && kpath[3]=='o' &&
            kpath[4]=='c' && kpath[5]=='/')
            pname = kpath + 6;
        if (pname && procfs_open(pname)) { exists = 1; goto found; }
    }
    /* Device special files and virtual dirs */
    {
        static const char *devpaths[] = {
            "/dev/null", "/dev/zero", "/dev/urandom", "/dev/tty",
            "/dev/console", "/dev", "/proc", 0
        };
        for (const char **dp = devpaths; *dp; dp++) {
            const char *a = kpath, *b = *dp;
            while (*a && *a == *b) { a++; b++; }
            if (*a == 0 && *b == 0) { exists = 1; goto found; }
        }
        /* /dev/tty1-tty4 */
        if (kpath[0]=='/' && kpath[1]=='d' && kpath[2]=='e' && kpath[3]=='v' &&
            kpath[4]=='/' && kpath[5]=='t' && kpath[6]=='t' && kpath[7]=='y' &&
            kpath[8] >= '1' && kpath[8] <= '9') {
            int vt_num = 0;
            const char *d = kpath + 8;
            while (*d >= '0' && *d <= '9') vt_num = vt_num * 10 + (*d++ - '0');
            if (*d == '\0' && vt_num >= 1 && vt_num <= PTY_MAX)
                { exists = 1; goto found; }
        }
        /* /dev/pts/0-3 */
        if (kpath[0]=='/' && kpath[1]=='d' && kpath[2]=='e' && kpath[3]=='v' &&
            kpath[4]=='/' && kpath[5]=='p' && kpath[6]=='t' && kpath[7]=='s' &&
            kpath[8]=='/') {
            int pts_id = 0;
            const char *d = kpath + 9;
            if (*d >= '0' && *d <= '9') {
                while (*d >= '0' && *d <= '9') pts_id = pts_id * 10 + (*d++ - '0');
                if (*d == '\0' && pts_id >= 0 && pts_id < PTY_MAX)
                    { exists = 1; goto found; }
            }
        }
    }
    /* ext2 on disk */
    {
        extern uint64_t ext2_walk_path(const char *);
        if (ext2_walk_path(kpath)) { exists = 1; goto found; }
    }

found:
    if (!exists) {
        if (vfs_err && vfs_err != -ENOENT) return vfs_err;
        return -ENOENT;
    }
    /* F_OK: existence only — already confirmed.
     * R_OK/W_OK/X_OK: single-user system, all permissions granted if file exists. */
    return 0;
}

/* ── SYS_statfs (137) / SYS_fstatfs (138) ───────── */

struct k_statfs {
    long f_type;     long f_bsize;
    long f_blocks;   long f_bfree;   long f_bavail;
    long f_files;    long f_ffree;
    struct { int __val[2]; } f_fsid;
    long f_namelen;  long f_frsize;
    long f_flags;    long f_spare[4];
};

static long fill_statfs(struct k_statfs *kbuf, const char *path) {
    kmemset(kbuf, 0, sizeof(*kbuf));
    kbuf->f_bsize = 4096;
    kbuf->f_frsize = 4096;
    kbuf->f_namelen = 255;

    /* Check filesystem type by path */
    int is_proc = (path && path[0]=='/' && path[1]=='p' && path[2]=='r' &&
                   path[3]=='o' && path[4]=='c' && (path[5]=='/' || path[5]==0));
    int is_tmp  = (path && path[0]=='/' && path[1]=='t' && path[2]=='m' &&
                   path[3]=='p' && (path[4]=='/' || path[4]==0));
    int is_dev  = (path && path[0]=='/' && path[1]=='d' && path[2]=='e' &&
                   path[3]=='v' && (path[4]=='/' || path[4]==0));

    if (is_proc) {
        kbuf->f_type = 0x9FA0; /* PROC_SUPER_MAGIC */
        kbuf->f_blocks = 0;
        kbuf->f_bfree = 0;
        kbuf->f_bavail = 0;
    } else if (is_tmp || is_dev) {
        kbuf->f_type = 0x01021994; /* TMPFS_MAGIC */
        kbuf->f_blocks = 1024;
        kbuf->f_bfree = 512;
        kbuf->f_bavail = 512;
    } else {
        /* CosmoFS */
        kbuf->f_type = 0x434F534D; /* 'COSM' */
        kbuf->f_blocks = (long)(page_alloc_total());
        kbuf->f_bfree = (long)(page_alloc_free());
        kbuf->f_bavail = kbuf->f_bfree;
    }
    kbuf->f_files = 1024;
    kbuf->f_ffree = 512;
    return 0;
}

long do_statfs(const char *path, void *buf) {
    char kpath[256], rpath[256];
    int r = copy_path_from_user(kpath, path, 256);
    if (r < 0) return r;
    resolve_path(kpath, rpath, 256);
    struct k_statfs kbuf;
    fill_statfs(&kbuf, rpath);
    return copy_to_user(buf, &kbuf, sizeof(kbuf)) ? -EFAULT : 0;
}

long do_fstatfs(int fd, void *buf) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type == FD_NONE) return -EBADF;
    struct k_statfs kbuf;
    /* Infer filesystem from fd type */
    const char *pseudo_path = "/";
    if (fde->type == FD_PROCFS) pseudo_path = "/proc";
    fill_statfs(&kbuf, pseudo_path);
    return copy_to_user(buf, &kbuf, sizeof(kbuf)) ? -EFAULT : 0;
}

/* ── SYS_statx (332) ─────────────────────────────── */

struct statx_timestamp {
    int64_t  tv_sec;
    uint32_t tv_nsec;
    int32_t  __reserved;
};

struct statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __spare0;
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct statx_timestamp stx_atime;
    struct statx_timestamp stx_btime;
    struct statx_timestamp stx_ctime;
    struct statx_timestamp stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint64_t __spare2[13];
};

#define STATX_BASIC_STATS 0x7ffU

long do_statx(int dirfd, const char *pathname, int flags,
              unsigned int mask, void *statxbuf) {
    (void)mask;
    if (!user_ok((uint64_t)statxbuf, sizeof(struct statx))) return -EFAULT;

    /* Get k_stat via existing fstatat path */
    struct k_stat kst;
    kmemset(&kst, 0, sizeof(kst));

    long r;
    if (flags & AT_EMPTY_PATH) {
        /* statx(fd, "", AT_EMPTY_PATH, ...) → fstat */
        r = vfs_fstat(dirfd, &kst);
    } else {
        char kpath[PATH_MAX], rpath[PATH_MAX];
        int len = resolve_at_path(dirfd, pathname, kpath, PATH_MAX);
        if (len < 0) return len;
        resolve_path(kpath, rpath, PATH_MAX);
        if (flags & AT_SYMLINK_NOFOLLOW)
            r = vfs_lstat(rpath, &kst);
        else
            r = vfs_stat(rpath, &kst);
    }
    if (r < 0) return r;

    /* Convert k_stat → statx */
    struct statx sx;
    kmemset(&sx, 0, sizeof(sx));
    sx.stx_mask     = STATX_BASIC_STATS;
    sx.stx_blksize  = (uint32_t)kst.st_blksize;
    sx.stx_nlink    = (uint32_t)kst.st_nlink;
    sx.stx_uid      = kst.st_uid;
    sx.stx_gid      = kst.st_gid;
    sx.stx_mode     = (uint16_t)kst.st_mode;
    sx.stx_ino      = kst.st_ino;
    sx.stx_size     = (uint64_t)kst.st_size;
    sx.stx_blocks   = (uint64_t)kst.st_blocks;
    sx.stx_atime.tv_sec  = kst.st_atime_sec;
    sx.stx_atime.tv_nsec = (uint32_t)kst.st_atime_nsec;
    sx.stx_btime.tv_sec  = kst.st_ctime_sec;  /* no btime — use ctime */
    sx.stx_btime.tv_nsec = (uint32_t)kst.st_ctime_nsec;
    sx.stx_ctime.tv_sec  = kst.st_ctime_sec;
    sx.stx_ctime.tv_nsec = (uint32_t)kst.st_ctime_nsec;
    sx.stx_mtime.tv_sec  = kst.st_mtime_sec;
    sx.stx_mtime.tv_nsec = (uint32_t)kst.st_mtime_nsec;
    sx.stx_dev_major = (uint32_t)(kst.st_dev >> 8);
    sx.stx_dev_minor = (uint32_t)(kst.st_dev & 0xff);
    sx.stx_rdev_major = (uint32_t)(kst.st_rdev >> 8);
    sx.stx_rdev_minor = (uint32_t)(kst.st_rdev & 0xff);

    return copy_to_user(statxbuf, &sx, sizeof(sx));
}

/* ── SYS_chown (92) — set file owner (single-user: noop, validate path) ── */

long do_chown(const char *upath, uint32_t uid, uint32_t gid) {
    char kpath[PATH_MAX];
    int len = copy_path_from_user(kpath, upath, PATH_MAX);
    if (len < 0) return len;
    char rpath[PATH_MAX];
    resolve_path(kpath, rpath, PATH_MAX);
    return vfs_chown(rpath, uid, gid);
}

/* ── SYS_fchownat (260) — chown with dirfd ────────── */

long do_fchownat(int dirfd, const char *upath, uint32_t uid, uint32_t gid, int flags) {
    (void)flags;
    char kpath[PATH_MAX];
    int len = resolve_at_path(dirfd, upath, kpath, PATH_MAX);
    if (len < 0) return len;
    char rpath[PATH_MAX];
    resolve_path(kpath, rpath, PATH_MAX);
    return vfs_chown(rpath, uid, gid);
}

/* ── SYS_renameat (264) — delegate to renameat2 ──── */

long do_renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath) {
    return do_renameat2(olddirfd, oldpath, newdirfd, newpath, 0);
}

/* ── SYS_faccessat2 (439) — delegate to faccessat ── */

long do_faccessat2(int dirfd, const char *path, int mode, int flags) {
    return do_faccessat(dirfd, path, mode, flags);
}
