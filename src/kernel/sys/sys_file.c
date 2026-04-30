/* CosmoRT Syscall Layer — file I/O syscalls */

#include "internal.h"
#include "core/waitqueue.h"
#include "core/time_ns.h"
#include "event/epoll.h"
#include "fs/loop.h"
#include "linux/capability.h"

/* Resolve a relative path against CWD, handling "." and ".." components.
 * chroot root (p->root) prepended to absolute paths; ".." above root
 * stays at root (Linux fs/namei.c path_init). */
int resolve_path(const char *path, char *out, int outsize) {
    if (!path || !out || outsize < 2) return -EINVAL;

    process_t *p = proc_current();
    int oi = 0;
    if (path[0] != '/') {
        const char *cwd = p ? p->cwd : "/";
        while (*cwd && oi < outsize - 1) out[oi++] = *cwd++;
        if (oi > 1 && out[oi - 1] != '/' && oi < outsize - 1) out[oi++] = '/';
    }
    while (*path && oi < outsize - 1) out[oi++] = *path++;
    out[oi] = '\0';

    /* Normalize: collapse "//", resolve "/." and "/.." in-place.
     * Rules match r[0]=='/' — don't consume the leading '/', let the loop
     * handle it so "/." / "/.." as first component also normalize
     * (e.g. CWD="/" + path="." → "/"). */
    char *w = out, *r = out;
    while (*r) {
        if (r[0] == '/' && r[1] == '/') {
            r++; /* skip duplicate slash */
        } else if (r[0] == '/' && r[1] == '.' && (r[2] == '/' || r[2] == '\0')) {
            r += 2; /* skip "/." */
        } else if (r[0] == '/' && r[1] == '.' && r[2] == '.' && (r[3] == '/' || r[3] == '\0')) {
            r += 3; /* skip "/.." */
            if (w > out + 1) { w--; while (w > out + 1 && w[-1] != '/') w--; }
        } else {
            *w++ = *r++;
        }
    }
    if (w == out) *w++ = '/'; /* root */
    if (w > out + 1 && w[-1] == '/') w--;
    *w = '\0';

    /* Apply chroot: prepend p->root to the absolute path */
    if (p && p->root[0]) {
        char buf[PATH_MAX];
        int bi = 0;
        const char *rp = p->root;
        while (*rp && bi < PATH_MAX - 1) buf[bi++] = *rp++;
        /* out is "/foo"; just concatenate */
        const char *op = out;
        if (op[0] == '/' && op[1] == '\0') {
            /* out is exactly "/" → result is p->root */
        } else {
            while (*op && bi < PATH_MAX - 1) buf[bi++] = *op++;
        }
        buf[bi] = '\0';
        int ci = 0;
        while (buf[ci] && ci < outsize - 1) { out[ci] = buf[ci]; ci++; }
        out[ci] = '\0';
    }
    return 0;
}

/* Resolve dirfd + relative path.
 * Absolute paths and AT_FDCWD are handled directly.
 * Real dirfd with relative path → prepend directory path from open fd. */
int resolve_at_path(int dirfd, const char *upath, char *kpath, int max) {
    int len = copy_path_from_user(kpath, upath, (size_t)max);
    if (len < 0) return len;
    if (kpath[0] == '/') return len;

    /* Relative path with AT_FDCWD: resolve against process CWD */
    if (dirfd == AT_FDCWD) {
        char tmp[PATH_MAX];
        for (int i = 0; i <= len && i < PATH_MAX; i++) tmp[i] = kpath[i];
        return resolve_path(tmp, kpath, max);
    }

    /* Real dirfd: look up the directory's path from the open vfs_file */
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(p->fds, dirfd);
    if (!fde) return -EBADF;
    if (fde->type != FD_FILE) return -ENOTDIR;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f || f->type != VFS_DIR) return -ENOTDIR;
    if (!f->path[0]) return -EBADF;

    /* Build absolute path: dirpath + "/" + relative */
    char tmp[PATH_MAX];
    int di = 0;
    const char *dp = f->path;
    while (*dp && di < PATH_MAX - 2) tmp[di++] = *dp++;
    if (di > 0 && tmp[di - 1] != '/') tmp[di++] = '/';
    const char *rp = kpath;
    while (*rp && di < PATH_MAX - 1) tmp[di++] = *rp++;
    tmp[di] = '\0';

    /* Copy back and normalize via resolve_path */
    return resolve_path(tmp, kpath, max);
}

/* Device file IDs (must match vfs.c) */
#define DEV_NULL    1
#define DEV_ZERO    2
#define DEV_URANDOM 3
#define DEV_TTY     4

__attribute__((hot))
long do_write(int fd, const void *buf, size_t count) {
    if (__builtin_expect(!count, 0)) return 0;
    if (__builtin_expect(!user_ok((uint64_t)buf, count), 0)) return -EFAULT;
    process_t *p = proc_current();
    if (__builtin_expect(!p, 0)) return -EFAULT;
    fd_entry_t *fde = fd_get(p->fds, fd);
    if (__builtin_expect(!fde, 0)) return -EBADF;
    if (__builtin_expect((fde->flags & O_ACCMODE) == O_RDONLY, 0)) return -EBADF;
    if (fde->type == FD_DEVICE) {
        int devid = (int)(uintptr_t)fde->obj;
        if (devid == DEV_NULL || devid == DEV_ZERO || devid == DEV_URANDOM)
            return (long)count; /* discard */
        if (devid >= DEV_LOOP_BASE && devid < DEV_LOOP_END)
            return loop_write(devid, buf, count, 0);
        if (devid == DEV_TTY)  { /* write to serial */
            size_t actual = count > 0x10000 ? 0x10000 : count;
            uint8_t kbuf[256]; size_t pos = 0;
            while (pos < actual) {
                size_t chunk = actual - pos > 256 ? 256 : actual - pos;
                copy_from_user(kbuf, (const uint8_t *)buf + pos, chunk);
                for (size_t j = 0; j < chunk; j++) serial_putchar((char)kbuf[j]);
                pos += chunk;
            }
            return (long)actual;
        }
        return -EBADF;
    }
    if (fde->type == FD_SERIAL) {
        size_t actual = count > 0x10000 ? 0x10000 : count;
        uint8_t kbuf[256];
        size_t pos = 0;
        while (pos < actual) {
            size_t chunk = actual - pos > 256 ? 256 : actual - pos;
            copy_from_user(kbuf, (const uint8_t *)buf + pos, chunk);
            for (size_t j = 0; j < chunk; j++) serial_putchar((char)kbuf[j]);
            pos += chunk;
        }
        return (long)actual;
    }
    if (fde->type == FD_FILE)
        return vfs_write(fd, buf, count);
    if (fde->type == FD_PROCFS) {
        procfs_fd_t *pf = (procfs_fd_t *)fde->obj;
        if (!pf) return -EACCES;
        /* handle == -2: per-PID dynamic file (e.g. /proc/123/oom_score_adj).
         * handle  >= 1: registered procfs entry. handle == -1/-3 (root,
         *   fd dir) sind nicht beschreibbar. */
        if (pf->handle != -2 && pf->handle < 1) return -EACCES;
        size_t actual = count > 0x10000 ? 0x10000 : count;
        char kbuf[512];
        size_t total = 0;
        while (total < actual) {
            size_t chunk = actual - total > sizeof(kbuf) ? sizeof(kbuf) : actual - total;
            int cr = copy_from_user(kbuf, (const char *)buf + total, chunk);
            if (cr) return cr;
            long w = (pf->handle == -2)
                ? procfs_pid_write(pf->name, kbuf, (int)chunk, pf->offset)
                : procfs_write(pf->handle, kbuf, (int)chunk, pf->offset);
            if (w < 0) return total ? (long)total : w;
            pf->offset += (int)w;
            total += (size_t)w;
            if ((size_t)w < chunk) break;
        }
        return (long)total;
    }
    if (fde->type == FD_SOCKET)
        return socket_write(fd, buf, (long)count);
    if (fde->type == FD_UNIX_SOCK) {
        long r = usock_write(fd, buf, (long)count);
        if (r != -EAGAIN) return r;
        if (fde->flags & O_NONBLOCK) return -EAGAIN;
        extern long usock_write_blocking(unix_socket_t *s, const void *buf, long count);
        unix_socket_t *us = usock_from_fd(fd);
        if (!us) return -EBADF;
        return usock_write_blocking(us, buf, (long)count);
    }
    if (fde->type == FD_PIPE) {
        int is_write = 0;
        struct pipe *pp = pipe_from_fd(fde, &is_write);
        if (!pp || !is_write) return -EBADF;
        long r = pipe_write(pp, buf, count);
        if (r != -EAGAIN) return r;
        /* Pipe full — block until reader drains */
        if (fde->flags & O_NONBLOCK) return -EAGAIN;
        return pipe_write_blocking(pp, buf, count);
    }
    if (fde->type == FD_EVENTFD)
        return eventfd_write(fde->obj, buf, (long)count, fde->flags & O_NONBLOCK);
    if (fde->type == FD_PTY_SLAVE) {
        int pty_id = (int)(long)fde->obj;
        uint8_t kbuf[256];
        size_t pos = 0;
        size_t actual = count > 0x10000 ? 0x10000 : count;
        while (pos < actual) {
            size_t chunk = actual - pos > 256 ? 256 : actual - pos;
            copy_from_user(kbuf, (const uint8_t *)buf + pos, chunk);
            int w = pty_slave_write(pty_id, (const char *)kbuf, (int)chunk);
            if (w <= 0) break;
            pos += (size_t)w;
        }
        /* Flush VT output for immediate rendering */
        vt_flush(pty_id);
        return (long)pos;
    }
    return -EBADF;
}

/* ── SYS_pwritev / SYS_writev ───────────────────── */

long do_pwritev(int fd, const struct iovec *iov, int iovcnt, int64_t offset) {
    if (iovcnt < 0 || iovcnt > 16) return -EINVAL;
    /* Copy iov array to kernel stack to prevent TOCTOU */
    struct iovec k_iov[16];
    { int r = copy_from_user(k_iov, iov, (size_t)iovcnt * sizeof(struct iovec)); if (r) return r; }

    /* If offset >= 0, use positional writes (seekable files only) */
    if (offset >= 0) {
        process_t *p = proc_current();
        if (__builtin_expect(!p, 0)) return -EFAULT;
        fd_entry_t *fde = fd_get(p->fds, fd);
        if (__builtin_expect(!fde, 0)) return -EBADF;
        if (fde->type != FD_FILE) return -ESPIPE;
        struct vfs_file *f = (struct vfs_file *)fde->obj;
        if (__builtin_expect(!f, 0)) return -EBADF;
        long total = 0;
        uint64_t pos = (uint64_t)offset;
        for (int i = 0; i < iovcnt; i++) {
            if (!user_ok((uint64_t)k_iov[i].iov_base, k_iov[i].iov_len)) return -EFAULT;
            long r = vfs_pwrite(f, (const void *)k_iov[i].iov_base, k_iov[i].iov_len, pos);
            if (r < 0) return total > 0 ? total : r;
            total += r;
            pos += (uint64_t)r;
            if ((size_t)r < k_iov[i].iov_len) break;
        }
        return total;
    }

    /* offset == -1: use current file position (works for all fd types) */
    long total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!user_ok((uint64_t)k_iov[i].iov_base, k_iov[i].iov_len)) return -EFAULT;
        long r = do_write(fd, (void *)k_iov[i].iov_base, k_iov[i].iov_len);
        if (r < 0) return total > 0 ? total : r;
        total += r;
        if ((size_t)r < k_iov[i].iov_len) break;
    }
    return total;
}

long do_writev(int fd, const struct iovec *iov, int iovcnt) {
    return do_pwritev(fd, iov, iovcnt, -1);
}

/* ── SYS_read (0) ────────────────────────────────── */

__attribute__((hot))
long do_read(int fd, void *buf, size_t count) {
    if (__builtin_expect(!user_ok((uint64_t)buf, count), 0)) return -EFAULT;
    process_t *p = proc_current();
    if (__builtin_expect(!p, 0)) return -EFAULT;
    fd_entry_t *fde = fd_get(p->fds, fd);
    if (__builtin_expect(!fde, 0)) return -EBADF;
    if (__builtin_expect((fde->flags & O_ACCMODE) == O_WRONLY, 0)) return -EBADF;
    if (fde->type == FD_DEVICE) {
        int devid = (int)(uintptr_t)fde->obj;
        if (devid == DEV_NULL)    return 0; /* EOF */
        if (devid == DEV_ZERO) {
            /* Fill with zeros */
            size_t actual = count > 0x10000 ? 0x10000 : count;
            kmemset(buf, 0, actual);
            return (long)actual;
        }
        if (devid == DEV_URANDOM) {
            /* Fill with random bytes */
            extern long do_getrandom(void *buf, size_t buflen, unsigned int flags);
            size_t actual = count > 4096 ? 4096 : count;
            return do_getrandom(buf, actual, 0);
        }
        if (devid >= DEV_LOOP_BASE && devid < DEV_LOOP_END)
            return loop_read(devid, buf, count, 0);
        return -EBADF;
    }
    if (fde->type == FD_SERIAL) {
        extern char serial_getchar(void);
        uint8_t kbuf[256];
        size_t got = 0;
        while (got < count && got < 256) {
            char c = serial_getchar();
            if (c == 0) break;
            kbuf[got++] = (uint8_t)c;
        }
        copy_to_user(buf, kbuf, got);
        return (long)got;
    }
    if (fde->type == FD_FILE)
        return vfs_read(fd, buf, count);
    if (fde->type == FD_PROCFS) {
        procfs_fd_t *pf = (procfs_fd_t *)fde->obj;
        if (!pf) return -EBADF;
        /* Read into kernel buffer, then copy to user */
        char kbuf[4096];
        int want = (int)count;
        if (want > (int)sizeof(kbuf)) want = (int)sizeof(kbuf);
        int got = (pf->handle == -2)
            ? procfs_pid_read(pf->name, kbuf, want, pf->offset)
            : procfs_read(pf->handle, kbuf, want, pf->offset);
        if (got > 0) {
            copy_to_user(buf, kbuf, (size_t)got);
            pf->offset += got;
        }
        return (long)got;
    }
    if (fde->type == FD_SOCKET)
        return socket_read(fd, buf, (long)count);
    if (fde->type == FD_UNIX_SOCK) {
        long r = usock_read(fd, buf, (long)count);
        if (r != -EAGAIN) return r;
        if (fde->flags & O_NONBLOCK) return -EAGAIN;
        /* Block until peer writes or closes */
        extern long usock_read_blocking(unix_socket_t *s, void *buf, long count);
        unix_socket_t *s = usock_from_fd(fd);
        if (!s) return -EBADF;
        return usock_read_blocking(s, buf, (long)count);
    }
    if (fde->type == FD_PIPE) {
        int is_write = 0;
        struct pipe *pp = pipe_from_fd(fde, &is_write);
        if (!pp || is_write) return -EBADF;
        /* Try non-blocking read first */
        long r = pipe_read(pp, buf, count);
        if (r != -EAGAIN) return r;
        /* No data available — check O_NONBLOCK */
        if (fde->flags & O_NONBLOCK) return -EAGAIN;
        /* Block until data arrives or write end closes */
        return pipe_read_blocking(pp, buf, count);
    }
    if (fde->type == FD_EVENTFD)
        return eventfd_read(fde->obj, buf, (long)count, fde->flags & O_NONBLOCK);
    if (fde->type == FD_TIMERFD)
        return timerfd_read(fde->obj, buf, (long)count, fde->flags & O_NONBLOCK);
    if (fde->type == FD_INOTIFY)
        return inotify_read(fde->obj, buf, (long)count);
    if (fde->type == FD_PTY_SLAVE) {
        int pty_id = (int)(long)fde->obj;
        pty_t *pty = pty_get(pty_id);
        if (!pty) return -EBADF;
        thread_t *t = thread_current();
        if (!t) return -EFAULT;

        uint8_t kbuf[256];
        size_t want = count > 256 ? 256 : count;
        extern void vt_flush(int vt_id);

        /* Fast path: data already pending. */
        int got = pty_slave_read(pty_id, (char *)kbuf, (int)want);
        if (got > 0) {
            copy_to_user(buf, kbuf, (size_t)got);
            vt_flush(pty_id);
            return (long)got;
        }

        /* Block on m2s_wq. prepare_to_wait serializes state-transition
         * and queue insertion under wq->lock; pty_master_write/_input_direct
         * call wake_up after dropping pty->lock, so no missed-wakeup. */
        DEFINE_WAIT(wait);
        long rc = 0;
        for (;;) {
            prepare_to_wait(&pty->m2s_wq, &wait, /*THREAD_BLOCKED*/ 3);

            got = pty_slave_read(pty_id, (char *)kbuf, (int)want);
            if (got > 0) { rc = got; break; }

            if (t->proc) {
                uint64_t deliverable = t->proc->sig_pending & ~t->sig_blocked;
                if (deliverable) { rc = -ERESTARTSYS; break; }
            }

            vt_flush(pty_id);
            schedule();
        }
        finish_wait(&pty->m2s_wq, &wait);

        if (rc > 0) {
            copy_to_user(buf, kbuf, (size_t)rc);
            vt_flush(pty_id);
        }
        return rc;
    }
    return -EBADF;
}

/* ── SYS_preadv / SYS_readv ─────────────────────── */

long do_preadv(int fd, const struct iovec *iov, int iovcnt, int64_t offset) {
    if (iovcnt < 0 || iovcnt > 1024) return -EINVAL;
    if (iovcnt > 64) return -EINVAL; /* kernel stack limit */
    /* Copy iovec array to kernel stack to prevent TOCTOU on iov_base/iov_len.
     * Buffer contents are still user memory — do_read validates via user_ok. */
    struct iovec kiov[64];
    { int r = copy_from_user(kiov, iov, (size_t)iovcnt * sizeof(struct iovec)); if (r) return r; }

    /* If offset >= 0, use positional reads (seekable files only) */
    if (offset >= 0) {
        process_t *p = proc_current();
        if (__builtin_expect(!p, 0)) return -EFAULT;
        fd_entry_t *fde = fd_get(p->fds, fd);
        if (__builtin_expect(!fde, 0)) return -EBADF;
        if (fde->type != FD_FILE) return -ESPIPE;
        struct vfs_file *f = (struct vfs_file *)fde->obj;
        if (__builtin_expect(!f, 0)) return -EBADF;
        long total = 0;
        uint64_t pos = (uint64_t)offset;
        for (int i = 0; i < iovcnt; i++) {
            if (!user_ok((uint64_t)kiov[i].iov_base, kiov[i].iov_len)) return -EFAULT;
            long r = vfs_pread(f, (void *)kiov[i].iov_base, kiov[i].iov_len, pos);
            if (r < 0) return total > 0 ? total : r;
            total += r;
            pos += (uint64_t)r;
            if ((size_t)r < kiov[i].iov_len) break; /* short read */
        }
        return total;
    }

    /* offset == -1: use current file position (works for all fd types) */
    long total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!user_ok((uint64_t)kiov[i].iov_base, kiov[i].iov_len)) return -EFAULT;
        long r = do_read(fd, (void *)kiov[i].iov_base, kiov[i].iov_len);
        if (r < 0) return total > 0 ? total : r;
        total += r;
        if ((size_t)r < kiov[i].iov_len) break; /* short read */
    }
    return total;
}

long do_readv(int fd, const struct iovec *iov, int iovcnt) {
    return do_preadv(fd, iov, iovcnt, -1);
}

/* ── SYS_close (3) ───────────────────────────────── */

/* Forward declarations for advisory file locking (defined below do_fcntl) */
static uint64_t flock_ino(fd_entry_t *fde);
void flock_release(uint64_t ino, uint32_t pid);

long do_close(int fd) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(p->fds, fd);
    if (!fde) return -EBADF;
    /* dnotify-Watches auf diesen fd freigeben (no-op wenn keiner). */
    dnotify_fd_closed(p, fd);
    if (fde->type == FD_FILE) {
        uint64_t ino = flock_ino(fde);
        if (ino) flock_release(ino, p->pid);
        return vfs_close(fd);
    }
    if (fde->type == FD_PROCFS) {
        procfs_fd_t *pf = (procfs_fd_t *)fde->obj;
        if (pf) {
            procfs_close(pf->handle);
            procfs_fd_free(pf);
        }
        return fd_close(p->fds, fd);
    }
    if (fde->type == FD_SOCKET)
        return socket_close(fd);
    if (fde->type == FD_UNIX_SOCK)
        return usock_close(fd);
    if (fde->type == FD_PIPE) {
        long r = pipe_close(fde);
        fd_close(p->fds, fd);
        return r;
    }
    if (fde->type == FD_EPOLL)   { epoll_destroy(fde->obj);   return fd_close(p->fds, fd); }
    if (fde->type == FD_EVENTFD) { eventfd_destroy(fde->obj); return fd_close(p->fds, fd); }
    if (fde->type == FD_TIMERFD) { timerfd_destroy(fde->obj); return fd_close(p->fds, fd); }
    if (fde->type == FD_INOTIFY) { inotify_destroy(fde->obj); return fd_close(p->fds, fd); }
    if (fde->type == FD_PTY_MASTER || fde->type == FD_PTY_SLAVE)
        return fd_close(p->fds, fd);
    if (fde->type == FD_NSFS) {
        nsfs_handle_free((struct nsfs_handle *)fde->obj);
        return fd_close(p->fds, fd);
    }
    return fd_close(p->fds, fd);
}

/* ── SYS_openat (257) — primary; SYS_open delegates with AT_FDCWD ── */

/* Return 1 if rpath equals "<prefix>/ns/{time,time_for_children,net}"
 * for the current process ("/proc/self/..." or "/proc/<own-pid>/..."),
 * else 0. Sets *kind to 0 for "time", 1 for "time_for_children",
 * 2 for "net". Unrecognised ns entries are left to vfs_open (-> ENOENT). */
static int rpath_is_ns_handle(const char *rpath, int *kind) {
    /* Only match /proc/ paths; caller has already fully canonicalised. */
    if (rpath[0] != '/' || rpath[1] != 'p' || rpath[2] != 'r' ||
        rpath[3] != 'o' || rpath[4] != 'c' || rpath[5] != '/') return 0;
    const char *p = rpath + 6;
    /* Accept "self/" */
    if (p[0]=='s' && p[1]=='e' && p[2]=='l' && p[3]=='f' && p[4]=='/') {
        p += 5;
    } else {
        /* Accept "<current-pid>/" — anything else we don't serve. */
        process_t *cur = proc_current();
        if (!cur) return 0;
        uint32_t pid = cur->pid, v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p++ - '0'); }
        if (*p != '/' || v != pid) return 0;
        p++;
    }
    if (!(p[0]=='n' && p[1]=='s' && p[2]=='/')) return 0;
    p += 3;
    if (p[0]=='t' && p[1]=='i' && p[2]=='m' && p[3]=='e') {
        if (p[4] == 0)        { *kind = 0; return 1; }
        if (p[4] == '_' && p[5]=='f' && p[6]=='o' && p[7]=='r' && p[8]=='_' &&
            p[9]=='c' && p[10]=='h' && p[11]=='i' && p[12]=='l' &&
            p[13]=='d' && p[14]=='r' && p[15]=='e' && p[16]=='n' && p[17]==0)
            { *kind = 1; return 1; }
    }
    if (p[0]=='n' && p[1]=='e' && p[2]=='t' && p[3]==0) { *kind = 2; return 1; }
    return 0;
}

long do_openat(int dirfd, const char *path, int flags, int mode) {
    char kpath[PATH_MAX], rpath[PATH_MAX];
    int len = resolve_at_path(dirfd, path, kpath, PATH_MAX);
    if (len < 0) return len;
    resolve_path(kpath, rpath, PATH_MAX);

    /* /proc/{self|pid}/ns/{time,time_for_children,net} — allocate a nsfs
     * handle. Linux: each kind captures the corresponding ns reference at
     * open-time. */
    int ns_kind = 0;
    if (rpath_is_ns_handle(rpath, &ns_kind)) {
        if (flags & (O_WRONLY | O_RDWR)) return -EINVAL;
        process_t *p = proc_current();
        if (!p) return -EFAULT;
        struct nsfs_handle *h = 0;
        if (ns_kind == NSFS_KIND_NET) {
            h = nsfs_handle_alloc_net(p->net_ns);
        } else {
            struct time_namespace *src =
                ns_kind == NSFS_KIND_TIME ? p->time_ns : p->time_ns_for_children;
            h = nsfs_handle_alloc(ns_kind, src);
        }
        if (!h) return -ENOMEM;
        int fd = fd_alloc(p->fds, FD_NSFS, h, flags);
        if (fd < 0) { nsfs_handle_free(h); return fd; }
        return fd;
    }

    return vfs_open(rpath, flags, mode);
}

long do_open(const char *path, int flags, int mode) {
    return do_openat(AT_FDCWD, path, flags, mode);
}

/* ── SYS_lseek (8) ──────────────────────────────── */

long do_lseek(int fd, long offset, int whence) {
    return vfs_lseek(fd, offset, whence);
}

/* ── SYS_dup3 (292) — primary; dup2 delegates here via dispatch ── */

long do_dup3(int oldfd, int newfd, int flags) {
    if (flags & ~O_CLOEXEC) return -EINVAL;
    if (oldfd == newfd) return -EINVAL;
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    if (oldfd < 0 || newfd < 0) return -EBADF;
    unsigned long nofile = p->rlim_nofile ? p->rlim_nofile : FD_DEFAULT_NOFILE;
    if ((unsigned long)oldfd >= nofile || (unsigned long)newfd >= nofile) return -EBADF;
    fd_entry_t *old = fd_get(p->fds, oldfd);
    if (!old) return -EBADF;
    /* Copy the entry to stack — subsequent table expansion may reallocate. */
    fd_entry_t old_copy = *old;

    /* Close newfd if open (must match do_close logic) */
    fd_entry_t *cur = fd_get(p->fds, newfd);
    if (cur) {
        if (cur->type == FD_FILE) {
            uint64_t ino = flock_ino(cur);
            if (ino) flock_release(ino, p->pid);
            vfs_close(newfd);
        } else if (cur->type == FD_PIPE) pipe_close(cur);
        else {
            fd_cleanup_entry(cur->type, cur->obj, cur->flags);
            fd_close(p->fds, newfd);
        }
        /* cur pointer still valid here; fd_install_at below may reallocate */
        fd_entry_t *slot = fd_entry_at(p->fds, newfd);
        if (slot) { slot->type = FD_NONE; slot->obj = 0; }
    }

    fd_entry_t installed = old_copy;
    installed.flags &= ~O_CLOEXEC;
    if (flags & O_CLOEXEC) installed.flags |= O_CLOEXEC;

    int r = fd_install_at(p->fds, newfd, installed);
    if (r < 0) return r;

    if (old_copy.type == FD_FILE && old_copy.obj) {
        extern void vfs_file_incref(struct vfs_file *f);
        vfs_file_incref((struct vfs_file *)old_copy.obj);
    } else if (old_copy.obj) {
        fd_obj_incref(old_copy.type, old_copy.obj, old_copy.flags);
    }
    return newfd;
}

/* ── SYS_getcwd (79) / SYS_chdir (80) ──────────── */

long do_getcwd(char *buf, size_t size) {
    if (!buf) return -EFAULT;
    if (size == 0) return -ERANGE;
    if (!user_ok((uint64_t)buf, size)) return -EFAULT;
    int r = vfs_getcwd(buf, size);
    if (r < 0) return r;
    return (long)(r + 1); /* Linux returns string length including NUL */
}

long do_chdir(const char *path) {
    char kpath[PATH_MAX], rpath[PATH_MAX];
    int len = copy_path_from_user(kpath, path, PATH_MAX);
    if (len < 0) return len;
    /* NAME_MAX check: any component > 255 chars → ENAMETOOLONG */
    int comp = 0;
    for (int i = 0; i < len; i++) {
        if (kpath[i] == '/') comp = 0;
        else if (++comp > 255) return -ENAMETOOLONG;
    }
    resolve_path(kpath, rpath, PATH_MAX);
    /* Linux fs/open.c sys_chdir → user_path_at(LOOKUP_FOLLOW|DIRECTORY)
     * → inode_permission(inode, MAY_EXEC). Non-root ohne x-Bit auf dem
     * Ziel bekommt EACCES. CAP_DAC_OVERRIDE/CAP_DAC_READ_SEARCH umgehen
     * den Check (Linux generic_permission). */
    process_t *p = proc_current();
    if (p && p->euid != 0 &&
        !(p->cap_effective & (CAP_TO_MASK(CAP_DAC_OVERRIDE) |
                              CAP_TO_MASK(CAP_DAC_READ_SEARCH)))) {
        struct k_stat st;
        int rc = vfs_stat(rpath, &st);
        if (rc < 0) return rc;
        if ((st.st_mode & S_IFMT) != S_IFDIR) return -ENOTDIR;
        rc = cred_may_access(p, st.st_uid, st.st_gid, st.st_mode, MAY_EXEC);
        if (rc < 0) return rc;
    }
    return vfs_chdir(rpath);
}

/* ── SYS_getdents64 (217) ───────────────────────── */

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[1]; /* flexible */
};

/* Emit one dirent into the output buffer. Returns bytes written or 0 if no space. */
static size_t emit_dirent(uint8_t *out, size_t remaining,
                          uint64_t ino, uint64_t off, uint8_t d_type,
                          const char *name) {
    int nlen = 0;
    while (name[nlen]) nlen++;
    /* d_reclen: header (19 bytes) + name + NUL, rounded up to 8 */
    size_t reclen = (19 + (size_t)nlen + 1 + 7) & ~(size_t)7;
    if (reclen > remaining) return 0;

    struct linux_dirent64 *ent = (struct linux_dirent64 *)out;
    ent->d_ino = ino;
    ent->d_off = (int64_t)off;
    ent->d_reclen = (uint16_t)reclen;
    ent->d_type = d_type;
    for (int i = 0; i < nlen; i++)
        ((char *)ent + 19)[i] = name[i];
    ((char *)ent + 19)[nlen] = 0;
    /* Zero padding */
    for (size_t i = 19 + (size_t)nlen + 1; i < reclen; i++)
        ((uint8_t *)ent)[i] = 0;
    return reclen;
}

/* Callback context for ext4 getdents64 via ext4_dir_iterate */
struct getdents_ctx {
    uint8_t *out;
    size_t   count;
    size_t   written;
    uint64_t next_off;
    int      full;       /* set when buffer is exhausted */
};

static int getdents_cb(const char *name, uint32_t ino, uint8_t file_type,
                       uint32_t next_pos, void *arg) {
    struct getdents_ctx *ctx = (struct getdents_ctx *)arg;
    /* Map ext4 file_type to DT_* */
    uint8_t d_type = 0; /* DT_UNKNOWN */
    if (file_type == EXT4_FT_REG_FILE) d_type = 8; /* DT_REG */
    else if (file_type == EXT4_FT_DIR) d_type = 4; /* DT_DIR */
    else if (file_type == EXT4_FT_SYMLINK) d_type = 10; /* DT_LNK */
    else d_type = 8; /* default to DT_REG */

    size_t n = emit_dirent(ctx->out + ctx->written, ctx->count - ctx->written,
                           ino, (uint64_t)next_pos, d_type, name);
    if (n == 0) { ctx->full = 1; return 1; /* stop — don't advance offset */ }
    ctx->next_off = (uint64_t)next_pos;
    ctx->written += n;
    return 0; /* continue */
}

/* Callback for procfs directory enumeration */
struct procfs_getdents_ctx {
    uint8_t *out;
    size_t   count;
    size_t   written;
    uint64_t next_off;
};

static int procfs_getdents_cb(const char *name, void *arg) {
    struct procfs_getdents_ctx *ctx = (struct procfs_getdents_ctx *)arg;
    uint64_t new_off = ctx->next_off + 1;
    size_t n = emit_dirent(ctx->out + ctx->written, ctx->count - ctx->written,
                           new_off, new_off, 8 /* DT_REG */, name);
    if (n == 0) return 1; /* stop: buffer full */
    ctx->next_off = new_off;
    ctx->written += n;
    return 0;
}

long do_getdents64(int fd, void *buf, size_t count) {
    if (!user_ok((uint64_t)buf, count)) return -EFAULT;

    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(p->fds, fd);
    if (!fde) return -EBADF;

    /* /proc directory (FD_PROCFS with handle == -1) */
    if (fde->type == FD_PROCFS) {
        procfs_fd_t *pf = (procfs_fd_t *)fde->obj;
        if (!pf) return -EBADF;

        /* /proc/self/fd directory (handle == -3) */
        if (pf->handle == -3) {
            struct procfs_getdents_ctx ctx = {
                .out = (uint8_t *)buf,
                .count = count,
                .written = 0,
                .next_off = (uint64_t)pf->offset
            };
            for (int i = pf->offset; i < p->fds->max_slots; i++) {
                fd_entry_t *e = fd_get(p->fds, i);
                if (!e || e->type == FD_NONE) continue;
                char name[12];
                int ni = 0;
                { int v = i; char t[12]; int ti = 0;
                  do { t[ti++] = '0' + (char)(v % 10); v /= 10; } while (v);
                  while (ti--) name[ni++] = t[ti]; }
                name[ni] = 0;
                ctx.next_off = (uint64_t)(i + 1);
                size_t n = emit_dirent(ctx.out + ctx.written, ctx.count - ctx.written,
                                       (uint64_t)(i + 1), ctx.next_off, 10 /* DT_LNK */, name);
                if (n == 0) break;
                ctx.written += n;
            }
            pf->offset = (int)ctx.next_off;
            return (long)ctx.written;
        }

        if (pf->handle != -1) return -ENOTDIR;
        struct procfs_getdents_ctx ctx = {
            .out = (uint8_t *)buf,
            .count = count,
            .written = 0,
            .next_off = (uint64_t)pf->offset
        };
        procfs_iterate(pf->offset, procfs_getdents_cb, &ctx);
        pf->offset = (int)ctx.next_off;
        return (long)ctx.written;
    }

    if (fde->type != FD_FILE) return -EBADF;

    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;

    /* ext4 directory */
    if (f->backend == VFS_BACKEND_EXT4) {
        if (f->type != VFS_DIR) return -ENOTDIR;
        struct getdents_ctx ctx = {
            .out = (uint8_t *)buf,
            .count = count,
            .written = 0,
            .next_off = f->offset,
            .full = 0
        };
        ext4_dir_iterate((uint32_t)f->disk_ino, (uint32_t)f->offset, getdents_cb, &ctx);
        f->offset = ctx.next_off;
        return (long)ctx.written;
    }

    /* ramfs directory */
    if (!f->inode || f->inode->type != VFS_DIR) return -ENOTDIR;

    struct vfs_inode *dir_ino = f->inode;
    uint8_t *out = (uint8_t *)buf;
    size_t written = 0;

    /* Walk to the child at offset f->offset */
    struct vfs_node *child = dir_ino->children;
    uint64_t idx = 0;
    while (child && idx < f->offset) {
        child = child->next;
        idx++;
    }

    while (child) {
        uint8_t d_type = (child->inode->type == VFS_DIR) ? 4 : 8;
        size_t n = emit_dirent(out + written, count - written,
                               child->inode->ino, f->offset + 1, d_type, child->name);
        if (n == 0) break;
        written += n;
        f->offset++;
        child = child->next;
    }

    return (long)written;
}

/* ── SYS_ioctl (16) / SYS_fcntl (72) ────────────── */

#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCSCTTY  0x540E
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define FIONREAD   0x541B
#define TIOCNOTTY  0x5422
#define TIOCGSID   0x5429
#define TIOCOUTQ   0x5411
#define TIOCINQ    FIONREAD
#define TIOCGPTN   0x80045430
#define TIOCSPTLCK 0x40045431
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_GETOWN        9
#define F_SETOWN        8
#define F_GETPIPE_SZ    1032
#define F_SETPIPE_SZ    1031
#define F_DUPFD_CLOEXEC 1030

struct winsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };

long do_ioctl(int fd, unsigned long request, unsigned long arg) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(p->fds, fd);
    if (!fde) return -EBADF;

    /* Loop-device ioctls: FD_DEVICE with devid in loop range */
    if (fde->type == FD_DEVICE) {
        int devid = (int)(uintptr_t)fde->obj;
        if (devid == DEV_LOOP_CTL ||
            (devid >= DEV_LOOP_BASE && devid < DEV_LOOP_END))
            return loop_ioctl(devid, request, arg);
    }

    if (request == TCGETS) {
        /* Return current PTY termios state. musl uses kernel struct termios
         * (36 bytes: 4×uint32 flags + c_line + c_cc[19]). */
        if (fde->type == FD_SERIAL) {
            if (!user_ok(arg, 36)) return -EFAULT;
            /* Static termios for serial console */
            struct kernel_termios st;
            kmemset(&st, 0, sizeof(st));
            st.c_iflag = ICRNL | IXON;
            st.c_oflag = OPOST | ONLCR;
            st.c_cflag = B38400 | CS8 | CREAD;
            st.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;
            return copy_to_user((void *)arg, &st, 36);
        }
        if (fde->type == FD_PTY_SLAVE || fde->type == FD_PTY_MASTER) {
            pty_t *pt = pty_get((int)(long)fde->obj);
            if (!pt) return -ENOTTY;
            if (!user_ok(arg, 36)) return -EFAULT;
            return copy_to_user((void *)arg, &pt->termios, 36);
        }
        return -ENOTTY;
    }
    if (request == FIONREAD) {
        if (!user_ok(arg, 4)) return -EFAULT;
        *(int *)arg = 0;
        return 0;
    }
    if (request == TIOCGWINSZ) {
        if (!user_ok(arg, sizeof(struct winsize))) return -EFAULT;
        struct winsize *ws = (struct winsize *)arg;
        if (fde->type == FD_PTY_SLAVE || fde->type == FD_PTY_MASTER) {
            pty_t *pt = pty_get((int)(long)fde->obj);
            if (pt) {
                ws->ws_row = pt->ws.ws_row;
                ws->ws_col = pt->ws.ws_col;
                ws->ws_xpixel = pt->ws.ws_xpixel;
                ws->ws_ypixel = pt->ws.ws_ypixel;
                return 0;
            }
        }
        if (fde->type == FD_SERIAL) {
            ws->ws_row = (uint16_t)vt_rows();
            ws->ws_col = (uint16_t)vt_cols();
            ws->ws_xpixel = 0;
            ws->ws_ypixel = 0;
            return 0;
        }
        return -ENOTTY;
    }
    if (request == TIOCSWINSZ) {
        if (!user_ok(arg, sizeof(struct winsize))) return -EFAULT;
        const struct winsize *nws = (const struct winsize *)arg;
        uint32_t fg_pgid = p->pgid;
        if (fde->type == FD_PTY_SLAVE || fde->type == FD_PTY_MASTER) {
            pty_t *pt = pty_get((int)(long)fde->obj);
            if (pt) {
                pt->ws.ws_row = nws->ws_row;
                pt->ws.ws_col = nws->ws_col;
                pt->ws.ws_xpixel = nws->ws_xpixel;
                pt->ws.ws_ypixel = nws->ws_ypixel;
                if (pt->fg_pgid > 0) fg_pgid = (uint32_t)pt->fg_pgid;
            }
        }
        do_kill(-(int)fg_pgid, SIGWINCH);
        return 0;
    }
    /* Terminal set: store full termios */
    if (request == TCSETS || request == TCSETSW || request == TCSETSF) {
        if (fde->type == FD_PTY_SLAVE || fde->type == FD_PTY_MASTER) {
            struct kernel_termios kterm;
            if (copy_from_user(&kterm, (const void *)arg, 36) == 0) {
                pty_t *pt = pty_get((int)(long)fde->obj);
                if (pt) {
                    uint64_t irqf;
                    spin_lock_irq(&pt->lock, &irqf);
                    int was_canon = (pt->termios.c_lflag & ICANON) != 0;
                    int new_canon = (kterm.c_lflag & ICANON) != 0;
                    /* Store complete termios */
                    kmemcpy(&pt->termios, &kterm, sizeof(struct kernel_termios));
                    /* Flush line buffer when switching canonical → raw */
                    if (was_canon && !new_canon && pt->line_pos > 0) {
                        for (int li = 0; li < pt->line_pos; li++) {
                            if (((pt->input_tail + 1) % PTY_BUF_SIZE) != pt->input_head)
                                pt->input_buf[pt->input_tail] = pt->line_buf[li],
                                pt->input_tail = (pt->input_tail + 1) % PTY_BUF_SIZE;
                        }
                        pt->line_pos = 0;
                    }
                    spin_unlock_irq(&pt->lock, irqf);
                }
            }
        }
        return 0;
    }
    /* Controlling terminal */
    if (request == TIOCSCTTY || request == TIOCNOTTY)
        return 0;
    /* Foreground process group: stored per-PTY */
    if (request == TIOCSPGRP) {
        int32_t pgid;
        { int r = copy_from_user(&pgid, (const void *)arg, 4); if (r) return r; }
        if (fde->type == FD_PTY_SLAVE || fde->type == FD_PTY_MASTER) {
            pty_t *pt = pty_get((int)(long)fde->obj);
            if (pt) pt->fg_pgid = pgid;
        }
        return 0;
    }
    if (request == TIOCGPGRP) {
        int32_t pgid = (int32_t)p->pgid;
        if (fde->type == FD_PTY_SLAVE || fde->type == FD_PTY_MASTER) {
            pty_t *pt = pty_get((int)(long)fde->obj);
            if (pt && pt->fg_pgid != 0)
                pgid = (int32_t)pt->fg_pgid;
        }
        return copy_to_user((void *)arg, &pgid, 4);
    }
    /* TIOCGSID: get session ID */
    if (request == TIOCGSID) {
        int32_t sid = (int32_t)p->sid;
        return copy_to_user((void *)arg, &sid, 4);
    }
    /* TIOCOUTQ: bytes in output queue (always 0 — we drain immediately) */
    if (request == TIOCOUTQ) {
        if (!user_ok(arg, 4)) return -EFAULT;
        *(int *)arg = 0;
        return 0;
    }
    /* FIONBIO: set/clear O_NONBLOCK */
    if (request == 0x5421 /* FIONBIO */) {
        if (!user_ok(arg, 4)) return -EFAULT;
        int on = *(int *)arg;
        if (on) fde->flags |= O_NONBLOCK;
        else    fde->flags &= ~O_NONBLOCK;
        return 0;
    }
    return -ENOTTY;
}

/* ── Advisory File Locking: POSIX + OFD + flock(2) + F_SETLEASE ──
 *
 * Eine Liste pro globalem Lock-Pool. Kein fixer Pool — slab-allokiert,
 * on-demand waechst mit Lock-Belastung, freigegeben bei close/exit. Lookups
 * sind linear, aber Workload ist typischerweise <16 aktive Locks pro Inode.
 *
 * Ein Entry deckt alle drei Lock-Arten ab, diskriminiert ueber `kind`:
 *   FL_POSIX  - fcntl byte-range (F_SETLK), keyed by pid
 *   FL_OFD    - fcntl OFD lock (F_OFD_SETLK), keyed by vfs_file*
 *   FL_FLOCK  - flock(2) whole-file, keyed by vfs_file*
 *   FL_LEASE  - F_SETLEASE, keyed by vfs_file*
 */

#define FL_POSIX  1
#define FL_OFD    2
#define FL_FLOCK  3
#define FL_LEASE  4

#define FLOCK_OFF_MAX __LONG_MAX__

struct flock_entry {
    struct flock_entry *next;
    uint64_t ino;
    uint32_t pid;         /* FL_POSIX owner */
    void    *owner;       /* FL_OFD/FL_FLOCK/FL_LEASE owner: vfs_file* */
    short    kind;
    short    type;        /* F_RDLCK or F_WRLCK (FL_LEASE uses both) */
    long     start;
    long     end;         /* inclusive end; FLOCK_OFF_MAX = EOF */
};

static slab_t     flock_slab;
static int        flock_slab_inited;
static struct flock_entry *flock_head;
static spinlock_t flock_lock = SPINLOCK_INIT;

static void flock_slab_ensure_init(void) {
    if (__builtin_expect(flock_slab_inited, 1)) return;
    slab_init_dynamic(&flock_slab, (int)sizeof(struct flock_entry), 8);
    flock_slab_inited = 1;
}

/* Stable inode identity from an fd entry (must be FD_FILE) */
static uint64_t flock_ino(fd_entry_t *fde) {
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return 0;
    if (f->disk_ino) return f->disk_ino;
    return (uint64_t)(uintptr_t)f->inode;
}

static int flock_range_overlap(long s1, long e1, long s2, long e2) {
    return s1 <= e2 && s2 <= e1;
}

/* Link/unlink helpers — caller holds flock_lock.
 * Insert sorted by (ino, start) so F_GETLK returns the lowest-offset
 * conflict first, matching Linux fs/locks.c:posix_lock_file ordering. */
static void flock_link(struct flock_entry *e) {
    struct flock_entry **pp = &flock_head;
    while (*pp) {
        struct flock_entry *c = *pp;
        if (c->ino > e->ino || (c->ino == e->ino && c->start > e->start)) break;
        pp = &c->next;
    }
    e->next = *pp;
    *pp = e;
}

static void flock_unlink(struct flock_entry *e) {
    struct flock_entry **pp = &flock_head;
    while (*pp && *pp != e) pp = &(*pp)->next;
    if (*pp) *pp = e->next;
}

static struct flock_entry *flock_new(void) {
    flock_slab_ensure_init();
    struct flock_entry *e = (struct flock_entry *)slab_alloc(&flock_slab);
    if (!e) return 0;
    e->next = 0;
    return e;
}

static void flock_drop(struct flock_entry *e) {
    flock_unlink(e);
    slab_free(&flock_slab, e);
}

/* Match helper: same lock-kind + same owner identity.
 * For FL_POSIX, owner is pid (task-level); dup/fork shares locks.
 * For FL_OFD/FL_FLOCK, owner is vfs_file* (file-description-level). */
static int flock_same_owner(const struct flock_entry *e, short kind,
                             uint32_t pid, void *owner) {
    if (e->kind != kind) return 0;
    if (kind == FL_POSIX) return e->pid == pid;
    return e->owner == owner;
}

/* Byte-range conflict check: shared-read is fine, everything else clashes.
 * POSIX and OFD conflict with each other (Linux kernel treats both against
 * the same per-inode lock table for conflict purposes). */
static struct flock_entry *flock_byterange_conflict(uint64_t ino,
                                                     short my_kind,
                                                     uint32_t my_pid,
                                                     void *my_owner,
                                                     short type,
                                                     long start, long end) {
    for (struct flock_entry *e = flock_head; e; e = e->next) {
        if (e->ino != ino) continue;
        if (e->kind != FL_POSIX && e->kind != FL_OFD) continue;
        if (flock_same_owner(e, my_kind, my_pid, my_owner)) continue;
        if (!flock_range_overlap(e->start, e->end, start, end)) continue;
        if (e->type == F_RDLCK && type == F_RDLCK) continue;
        return e;
    }
    return 0;
}

/* Remove/split existing byte-range locks (same owner) overlapping [start,end].
 * Linux: overlapping same-owner locks are consumed before re-lock. */
static int flock_byterange_unlock(uint64_t ino, short kind,
                                   uint32_t pid, void *owner,
                                   long start, long end) {
    struct flock_entry *e = flock_head, *next;
    while (e) {
        next = e->next;
        if (e->ino == ino && flock_same_owner(e, kind, pid, owner) &&
            flock_range_overlap(e->start, e->end, start, end)) {
            long es = e->start, ee = e->end;
            short et = e->type;
            if (es < start && ee > end) {
                e->end = start - 1;
                struct flock_entry *n = flock_new();
                if (!n) { e->end = ee; return -ENOLCK; }
                n->ino = ino; n->pid = pid; n->owner = owner;
                n->kind = kind; n->type = et;
                n->start = end + 1; n->end = ee;
                flock_link(n);
            } else if (es < start) {
                e->end = start - 1;
            } else if (ee > end) {
                e->start = end + 1;
            } else {
                flock_drop(e);
            }
        }
        e = next;
    }
    return 0;
}

/* Merge adjacent/overlapping same-owner same-type locks (Linux semantics) */
static void flock_byterange_merge(uint64_t ino, short kind,
                                   uint32_t pid, void *owner, short type) {
    struct flock_entry *a = flock_head;
    while (a) {
        if (a->ino == ino && a->type == type &&
            flock_same_owner(a, kind, pid, owner)) {
            struct flock_entry *b = a->next, *bn;
            while (b) {
                bn = b->next;
                if (b->ino == ino && b->type == type &&
                    flock_same_owner(b, kind, pid, owner) &&
                    !(a->end + 1 < b->start || b->end + 1 < a->start)) {
                    if (b->start < a->start) a->start = b->start;
                    if (b->end   > a->end)   a->end   = b->end;
                    flock_drop(b);
                }
                b = bn;
            }
        }
        a = a->next;
    }
}

static long flock_byterange_setlk(uint64_t ino, short kind,
                                   uint32_t pid, void *owner,
                                   short type, long start, long end) {
    if (type == F_UNLCK)
        return flock_byterange_unlock(ino, kind, pid, owner, start, end);
    if (flock_byterange_conflict(ino, kind, pid, owner, type, start, end))
        return -EAGAIN;
    int r = flock_byterange_unlock(ino, kind, pid, owner, start, end);
    if (r) return r;
    struct flock_entry *n = flock_new();
    if (!n) return -ENOLCK;
    n->ino = ino; n->pid = pid; n->owner = owner;
    n->kind = kind; n->type = type;
    n->start = start; n->end = end;
    flock_link(n);
    flock_byterange_merge(ino, kind, pid, owner, type);
    return 0;
}

/* flock(2) whole-file: keyed by vfs_file*. Distinct open(2) descriptions
 * conflict even within the same process. */
static struct flock_entry *flock_whole_conflict(uint64_t ino, void *owner,
                                                 short type) {
    for (struct flock_entry *e = flock_head; e; e = e->next) {
        if (e->ino != ino || e->kind != FL_FLOCK) continue;
        if (e->owner == owner) continue;
        if (e->type == F_RDLCK && type == F_RDLCK) continue;
        return e;
    }
    return 0;
}

static long flock_whole_setlk(uint64_t ino, uint32_t pid, void *owner,
                               short type) {
    if (flock_whole_conflict(ino, owner, type))
        return -EAGAIN;
    for (struct flock_entry *e = flock_head; e; e = e->next) {
        if (e->ino == ino && e->kind == FL_FLOCK && e->owner == owner) {
            e->type = type;
            return 0;
        }
    }
    struct flock_entry *n = flock_new();
    if (!n) return -ENOLCK;
    n->ino = ino; n->pid = pid; n->owner = owner;
    n->kind = FL_FLOCK; n->type = type;
    n->start = 0; n->end = FLOCK_OFF_MAX;
    flock_link(n);
    return 0;
}

static void flock_whole_unlock(uint64_t ino, void *owner) {
    struct flock_entry *e = flock_head, *next;
    while (e) {
        next = e->next;
        if (e->ino == ino && e->kind == FL_FLOCK && e->owner == owner)
            flock_drop(e);
        e = next;
    }
}

/* F_SETLEASE — per-vfs_file lease state. Linux semantics:
 *   F_WRLCK: exclusive lease, only the lease-holder may open writably
 *   F_RDLCK: shared lease, multiple readers ok, writes would break
 *   F_UNLCK: remove lease
 * We store state only; break-notification (SIGIO) is not wired up. */
static struct flock_entry *flock_lease_find(uint64_t ino, void *owner) {
    for (struct flock_entry *e = flock_head; e; e = e->next)
        if (e->ino == ino && e->kind == FL_LEASE && e->owner == owner)
            return e;
    return 0;
}

/* ── Deadlock-Detection fuer F_SETLKW ────────────
 *
 * Jeder Blocker-Wait traegt sich in flock_waiters ein: waiter_pid -> blocker_pid.
 * Vor dem Schlafen fuer (me -> target) prueft flock_deadlock ob target -> X
 * -> ... -> me existiert (Zyklus). Linux fs/locks.c:posix_locks_deadlock macht
 * das ebenfalls mit einem BFS im blocked_lock_list. */

#define FLOCK_DEADLOCK_MAX_DEPTH 32

struct flock_waiter {
    struct flock_waiter *next;
    uint32_t waiter_pid;   /* Prozess, der im F_SETLKW haengt */
    uint32_t blocker_pid;  /* Besitzer der blockierenden Lock */
};

static slab_t flock_waiter_slab;
static int flock_waiter_slab_inited;
static struct flock_waiter *flock_waiter_head;

static void flock_waiter_slab_ensure(void) {
    if (__builtin_expect(flock_waiter_slab_inited, 1)) return;
    slab_init_dynamic(&flock_waiter_slab, (int)sizeof(struct flock_waiter), 4);
    flock_waiter_slab_inited = 1;
}

/* Caller holds flock_lock. Liefert 1 wenn waiter_pid irgendwann ueber die
 * Wait-Kette wieder auf my_pid zeigt (Zyklus). FIFO-BFS, Depth-limit gegen
 * pathologische Graphen. */
static int flock_deadlock(uint32_t my_pid, uint32_t blocker_pid) {
    if (blocker_pid == 0) return 0;
    if (blocker_pid == my_pid) return 1;
    uint32_t cur = blocker_pid;
    for (int depth = 0; depth < FLOCK_DEADLOCK_MAX_DEPTH; depth++) {
        uint32_t next = 0;
        for (struct flock_waiter *w = flock_waiter_head; w; w = w->next) {
            if (w->waiter_pid == cur) { next = w->blocker_pid; break; }
        }
        if (!next) return 0;
        if (next == my_pid) return 1;
        cur = next;
    }
    return 0;
}

static struct flock_waiter *flock_waiter_add(uint32_t waiter_pid, uint32_t blocker_pid) {
    flock_waiter_slab_ensure();
    struct flock_waiter *w = (struct flock_waiter *)slab_alloc(&flock_waiter_slab);
    if (!w) return 0;
    w->waiter_pid = waiter_pid;
    w->blocker_pid = blocker_pid;
    w->next = flock_waiter_head;
    flock_waiter_head = w;
    return w;
}

static void flock_waiter_remove(struct flock_waiter *w) {
    if (!w) return;
    struct flock_waiter **pp = &flock_waiter_head;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp) *pp = w->next;
    slab_free(&flock_waiter_slab, w);
}

/* Remove all locks held by pid on a given inode — called on close(fd).
 * POSIX: close() releases all byte-range POSIX locks held by the calling
 * process on the underlying file. OFD/FLOCK locks persist until the last
 * vfs_file reference is dropped (see flock_release_file). */
void flock_release(uint64_t ino, uint32_t pid) {
    spin_lock(&flock_lock);
    struct flock_entry *e = flock_head, *next;
    while (e) {
        next = e->next;
        if (e->ino == ino && e->kind == FL_POSIX && e->pid == pid)
            flock_drop(e);
        e = next;
    }
    spin_unlock(&flock_lock);
}

/* Release OFD/FLOCK/LEASE locks when a struct vfs_file is finally freed */
void flock_release_file(void *vfs_file_ptr) {
    spin_lock(&flock_lock);
    struct flock_entry *e = flock_head, *next;
    while (e) {
        next = e->next;
        if (e->owner == vfs_file_ptr &&
            (e->kind == FL_OFD || e->kind == FL_FLOCK || e->kind == FL_LEASE))
            flock_drop(e);
        e = next;
    }
    spin_unlock(&flock_lock);
}

/* Remove all POSIX locks held by a process (called on exit) */
void flock_release_pid(uint32_t pid) {
    spin_lock(&flock_lock);
    struct flock_entry *e = flock_head, *next;
    while (e) {
        next = e->next;
        if (e->kind == FL_POSIX && e->pid == pid)
            flock_drop(e);
        e = next;
    }
    /* Stale Waiter-Eintraege fuer diesen pid entfernen (Deadlock-Graph). */
    struct flock_waiter **wp = &flock_waiter_head;
    while (*wp) {
        if ((*wp)->waiter_pid == pid) {
            struct flock_waiter *dead = *wp;
            *wp = dead->next;
            slab_free(&flock_waiter_slab, dead);
            continue;
        }
        wp = &(*wp)->next;
    }
    spin_unlock(&flock_lock);
}

/* ── SYS_flock (73) — whole-file advisory locking ── */

long do_flock(int fd, int operation) {
    int nb = (operation & LOCK_NB) ? 1 : 0;
    int op = operation & ~LOCK_NB;

    if (op != LOCK_SH && op != LOCK_EX && op != LOCK_UN) return -EINVAL;
    if (operation == LOCK_NB) return -EINVAL;

    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(p->fds, fd);
    if (!fde || fde->type == FD_NONE) return -EBADF;
    if (fde->type != FD_FILE) return -EINVAL;
    uint64_t ino = flock_ino(fde);
    if (!ino) return -EBADF;

    void *owner = fde->obj;

    if (op == LOCK_UN) {
        spin_lock(&flock_lock);
        flock_whole_unlock(ino, owner);
        spin_unlock(&flock_lock);
        return 0;
    }

    short t = (op == LOCK_SH) ? F_RDLCK : F_WRLCK;
    spin_lock(&flock_lock);
    long r = flock_whole_setlk(ino, p->pid, owner, t);
    spin_unlock(&flock_lock);
    if (r == -EAGAIN && nb) return -EAGAIN;
    if (r != -EAGAIN) return r;

    while (1) {
        thread_t *th = thread_current();
        if (th && th->proc) {
            uint64_t deliverable = (th->proc->sig_pending | th->sig_thread_pending) & ~th->sig_blocked;
            if (deliverable) return -ERESTARTSYS;
        }
        (void)sleep_interruptible_ns(10ULL * NSEC_PER_MSEC);
        spin_lock(&flock_lock);
        r = flock_whole_setlk(ino, p->pid, owner, t);
        spin_unlock(&flock_lock);
        if (r != -EAGAIN) return r;
    }
}

/* Resolve l_whence against current file offset / size.
 * Returns 0 on success, -EINVAL / -EOVERFLOW-equivalent on failure. */
static long flock_resolve_range(fd_entry_t *fde, const struct k_flock *fl,
                                 long *out_start, long *out_end) {
    long base = 0;
    if (fl->l_whence == SEEK_SET) {
        base = 0;
    } else if (fl->l_whence == SEEK_CUR) {
        if (fde->type != FD_FILE) return -EINVAL;
        struct vfs_file *f = (struct vfs_file *)fde->obj;
        if (!f) return -EBADF;
        base = (long)f->offset;
    } else if (fl->l_whence == SEEK_END) {
        if (fde->type != FD_FILE) return -EINVAL;
        struct vfs_file *f = (struct vfs_file *)fde->obj;
        if (!f) return -EBADF;
        struct k_stat st;
        if (f->f_ops && f->f_ops->fstat && f->f_ops->fstat(f, &st) == 0)
            base = (long)st.st_size;
        else
            base = (long)f->disk_size;
    } else {
        return -EINVAL;
    }

    long start = base + fl->l_start;
    if (start < 0) return -EINVAL;

    long end;
    if (fl->l_len == 0) {
        end = FLOCK_OFF_MAX;
    } else if (fl->l_len > 0) {
        end = start + fl->l_len - 1;
        if (end < start) return -EINVAL;
    } else {
        /* Negative l_len: lock the bytes preceding l_start (Linux allows). */
        end = start - 1;
        start = start + fl->l_len;
        if (start < 0) return -EINVAL;
    }

    *out_start = start;
    *out_end = end;
    return 0;
}

/* Common F_GETLK / F_SETLK / F_SETLKW dispatcher.
 * kind: FL_POSIX or FL_OFD. Returns long fcntl result or -errno. */
static long fcntl_do_lock(int cmd, short kind, fd_entry_t *fde, uint32_t pid,
                           long arg) {
    struct k_flock ufl;
    if (copy_from_user(&ufl, (void *)arg, sizeof(ufl)) < 0) return -EFAULT;

    int is_get = (cmd == F_GETLK || cmd == F_OFD_GETLK);
    int is_wait = (cmd == F_SETLKW || cmd == F_OFD_SETLKW);

    if (is_get) {
        if (ufl.l_type != F_RDLCK && ufl.l_type != F_WRLCK) return -EINVAL;
    } else {
        if (ufl.l_type != F_RDLCK && ufl.l_type != F_WRLCK &&
            ufl.l_type != F_UNLCK) return -EINVAL;
    }

    /* OFD locks require l_pid == 0 on input */
    if (kind == FL_OFD && ufl.l_pid != 0) return -EINVAL;

    /* Validate whence before file-type check: Linux fs/fcntl.c path
     * (flock_to_posix_lock) rejects bad whence with EINVAL on any fd. */
    if (ufl.l_whence != SEEK_SET && ufl.l_whence != SEEK_CUR &&
        ufl.l_whence != SEEK_END) return -EINVAL;

    if (fde->type != FD_FILE) return is_get ? -EBADF : -EINVAL;
    uint64_t ino = flock_ino(fde);
    if (!ino) return -EBADF;

    long start, end;
    long rr = flock_resolve_range(fde, &ufl, &start, &end);
    if (rr) return rr;

    void *owner = (kind == FL_OFD) ? fde->obj : 0;

    if (is_get) {
        spin_lock(&flock_lock);
        struct flock_entry *c = flock_byterange_conflict(ino, kind, pid, owner,
                                                          ufl.l_type, start, end);
        if (c) {
            ufl.l_type   = c->type;
            ufl.l_whence = SEEK_SET;
            ufl.l_start  = c->start;
            ufl.l_len    = (c->end == FLOCK_OFF_MAX) ? 0 : (c->end - c->start + 1);
            ufl.l_pid    = (c->kind == FL_OFD) ? -1 : (int)c->pid;
        } else {
            ufl.l_type = F_UNLCK;
            /* POSIX: leave l_whence/l_start/l_len/l_pid unchanged on no
             * conflict. Linux fs/locks.c:posix_test_lock only overwrites
             * fields when a conflict is found. */
        }
        spin_unlock(&flock_lock);
        if (copy_to_user((void *)arg, &ufl, sizeof(ufl)) < 0) return -EFAULT;
        return 0;
    }

    /* F_SETLK / F_SETLKW / F_OFD_SETLK / F_OFD_SETLKW */
    spin_lock(&flock_lock);
    long r = flock_byterange_setlk(ino, kind, pid, owner, ufl.l_type, start, end);
    spin_unlock(&flock_lock);
    if (!is_wait) return r;

    /* Waiter-Eintrag fuer Deadlock-Detection. POSIX-Lock-Konflikte werden
     * ueber pid tracked; OFD-Lock-Konflikte ebenfalls via pid des Haltenden
     * (owner-Kette zu pid im struct flock_entry mapped). */
    struct flock_waiter *waiter_slot = 0;

    while (r == -EAGAIN) {
        thread_t *th = thread_current();
        if (th && th->proc) {
            uint64_t deliverable = (th->proc->sig_pending | th->sig_thread_pending) & ~th->sig_blocked;
            if (deliverable) {
                if (waiter_slot) { spin_lock(&flock_lock); flock_waiter_remove(waiter_slot); spin_unlock(&flock_lock); }
                return -ERESTARTSYS;
            }
        }
        /* Vor dem Blocken: Deadlock-Check. Finde aktuellen Blocker, registriere
         * Wait-Edge, pruefe auf Zyklus (nur bei POSIX-Locks — OFD-Locks haben
         * keine Prozess-Identitaet in Linux deadlock-detection). */
        spin_lock(&flock_lock);
        struct flock_entry *c = flock_byterange_conflict(ino, kind, pid, owner,
                                                          ufl.l_type, start, end);
        /* Deadlock-Detection nur fuer POSIX-vs-POSIX-Konflikte. OFD-Locks
         * haben keine Prozess-Identitaet (mehrere OFD-Owner im selben pid),
         * und POSIX-Lock-Holder mit gleichem pid kollidieren nicht — beide
         * Faelle wuerden flock_deadlock(pid, pid==pid) false-positive. */
        if (c && kind == FL_POSIX && c->kind == FL_POSIX && c->pid != pid) {
            if (flock_deadlock(pid, c->pid)) {
                if (waiter_slot) flock_waiter_remove(waiter_slot);
                spin_unlock(&flock_lock);
                return -EDEADLK;
            }
            if (!waiter_slot) waiter_slot = flock_waiter_add(pid, c->pid);
            else waiter_slot->blocker_pid = c->pid;
        }
        spin_unlock(&flock_lock);

        (void)sleep_interruptible_ns(10ULL * NSEC_PER_MSEC);
        spin_lock(&flock_lock);
        r = flock_byterange_setlk(ino, kind, pid, owner, ufl.l_type, start, end);
        spin_unlock(&flock_lock);
    }
    if (waiter_slot) { spin_lock(&flock_lock); flock_waiter_remove(waiter_slot); spin_unlock(&flock_lock); }
    return r;
}

/* F_SETLEASE — Linux fs/locks.c:generic_add_lease:
 *   F_RDLCK: fails -EAGAIN if any writable opens of the inode exist.
 *   F_WRLCK: fails -EAGAIN if any other open of this inode exists
 *            (refcount > 1 means dup or fork shared this file-description).
 *   F_UNLCK: removes existing lease.
 * Lease-break SIGIO delivery is not wired up. */
static long fcntl_setlease(fd_entry_t *fde, long arg) {
    if (fde->type != FD_FILE) return -EINVAL;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;
    short t = (short)arg;
    if (t != F_RDLCK && t != F_WRLCK && t != F_UNLCK) return -EINVAL;
    int acc = f->flags & O_ACCMODE;
    if (t == F_WRLCK && f->refcount > 1) return -EAGAIN;
    /* F_RDLCK on a writable fd is always a conflict — even for the caller's
     * own open, since a reader-lease means "signal me if anyone opens for
     * write". Caller must open O_RDONLY to take a read-lease. */
    if (t == F_RDLCK && acc != O_RDONLY) return -EAGAIN;

    uint64_t ino = flock_ino(fde);
    if (!ino) return -EBADF;

    spin_lock(&flock_lock);
    struct flock_entry *e = flock_lease_find(ino, f);
    if (t == F_UNLCK) {
        if (e) flock_drop(e);
        spin_unlock(&flock_lock);
        return 0;
    }
    /* Only one lease per (file-description, inode) — downgrade/upgrade */
    if (e) {
        e->type = t;
        spin_unlock(&flock_lock);
        return 0;
    }
    struct flock_entry *n = flock_new();
    if (!n) { spin_unlock(&flock_lock); return -ENOLCK; }
    n->ino = ino; n->pid = 0; n->owner = f;
    n->kind = FL_LEASE; n->type = t;
    n->start = 0; n->end = FLOCK_OFF_MAX;
    flock_link(n);
    spin_unlock(&flock_lock);
    return 0;
}

static long fcntl_getlease(fd_entry_t *fde) {
    if (fde->type != FD_FILE) return -EINVAL;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;
    uint64_t ino = flock_ino(fde);
    if (!ino) return F_UNLCK;

    spin_lock(&flock_lock);
    struct flock_entry *e = flock_lease_find(ino, f);
    long r = e ? e->type : F_UNLCK;
    spin_unlock(&flock_lock);
    return r;
}

/* F_SETPIPE_SZ — delegiert an pipe_resize (sys_ipc.c). Linux-Semantik:
 *   arg >= (1<<31)      → -EINVAL
 *   arg > pipe-max-size → -EPERM (unprivileged user)
 *   new < current fill  → -EBUSY
 *   sonst: ring auf rounded-up-page-size kopieren, rounded-size returnen
 * F_GETPIPE_SZ liefert die aktuelle Ringgroesse. pipe-max-size ist
 * dynamisch via /proc/sys/fs/pipe-max-size. */
#define PIPE_BUF_MIN          4096      /* Linux: mindestens eine Seite */

long do_fcntl(int fd, int cmd, long arg) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(p->fds, fd);
    if (!fde) return -EBADF;

    switch (cmd) {
    case F_GETFL: return fde->flags & ~O_CLOEXEC;
    case F_SETFL: {
        int keep = fde->flags & (O_RDONLY | O_WRONLY | O_RDWR | O_CLOEXEC);
        int new_bits = (int)arg & (O_APPEND | O_NONBLOCK | O_ASYNC);
        fde->flags = keep | new_bits;
        /* O_ASYNC auf Pipe aktiviert SIGIO-Delivery via pipe_owner */
        if (fde->type == FD_PIPE) {
            int end = (fde->flags & O_WRONLY) ? 1 : 0;
            pipe_set_async((struct pipe *)fde->obj, end, !!(new_bits & O_ASYNC));
        }
        return 0;
    }
    case F_GETFD: return (fde->flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
    case F_SETFD: {
        if (arg & FD_CLOEXEC) fde->flags |= O_CLOEXEC;
        else fde->flags &= ~O_CLOEXEC;
        return 0;
    }
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
        return fcntl_do_lock(cmd, FL_POSIX, fde, p->pid, arg);
    case F_OFD_GETLK:
    case F_OFD_SETLK:
    case F_OFD_SETLKW:
        return fcntl_do_lock(cmd, FL_OFD, fde, p->pid, arg);
    case F_GETOWN:
        if (fde->type == FD_PIPE) {
            int end = (fde->flags & O_WRONLY) ? 1 : 0;
            return pipe_fcntl_getown((struct pipe *)fde->obj, end);
        }
        if (fde->type == FD_FILE && fde->obj)
            return ((struct vfs_file *)fde->obj)->f_owner;
        return 0;
    case F_SETOWN:
        if (fde->type == FD_PIPE) {
            int end = (fde->flags & O_WRONLY) ? 1 : 0;
            return pipe_fcntl_setown((struct pipe *)fde->obj, end, (int)arg);
        }
        if (fde->type == FD_FILE && fde->obj) {
            ((struct vfs_file *)fde->obj)->f_owner = (int)arg;
        }
        return 0;
    case F_GETOWN_EX: {
        struct k_f_owner_ex ex = { F_OWNER_PID, 0 };
        int pid = 0;
        if (fde->type == FD_PIPE) {
            int end = (fde->flags & O_WRONLY) ? 1 : 0;
            pid = (int)pipe_fcntl_getown((struct pipe *)fde->obj, end);
        } else if (fde->type == FD_FILE && fde->obj) {
            pid = ((struct vfs_file *)fde->obj)->f_owner;
        }
        if (pid < 0) { ex.type = F_OWNER_PGRP; ex.pid = -pid; }
        else         { ex.type = F_OWNER_PID;  ex.pid = pid; }
        if (copy_to_user((void *)arg, &ex, sizeof(ex)) < 0) return -EFAULT;
        return 0;
    }
    case F_SETOWN_EX: {
        struct k_f_owner_ex ex;
        if (copy_from_user(&ex, (void *)arg, sizeof(ex)) < 0) return -EFAULT;
        if (ex.type != F_OWNER_TID && ex.type != F_OWNER_PID &&
            ex.type != F_OWNER_PGRP) return -EINVAL;
        int pipe_type = (ex.type == F_OWNER_TID)  ? 2 /* PIPE_OWNER_TID  */
                     : (ex.type == F_OWNER_PGRP) ? 1 /* PIPE_OWNER_PGRP */
                                                  : 0 /* PIPE_OWNER_PID  */;
        if (fde->type == FD_PIPE) {
            int end = (fde->flags & O_WRONLY) ? 1 : 0;
            extern long pipe_fcntl_setown_ex(struct pipe *pp, int end, int who, int type);
            pipe_fcntl_setown_ex((struct pipe *)fde->obj, end, ex.pid, pipe_type);
        } else if (fde->type == FD_FILE && fde->obj) {
            int f_pid = ex.pid;
            if (ex.type == F_OWNER_PGRP) f_pid = -f_pid;
            ((struct vfs_file *)fde->obj)->f_owner = f_pid;
        }
        return 0;
    }
    case F_GETSIG:
        if (fde->type == FD_PIPE) {
            int end = (fde->flags & O_WRONLY) ? 1 : 0;
            return pipe_fcntl_getsig((struct pipe *)fde->obj, end);
        }
        if (fde->type == FD_FILE && fde->obj)
            return ((struct vfs_file *)fde->obj)->f_sig;
        return 0;
    case F_SETSIG:
        if ((int)arg < 0 || (int)arg > 64) return -EINVAL;
        if (fde->type == FD_PIPE) {
            int end = (fde->flags & O_WRONLY) ? 1 : 0;
            return pipe_fcntl_setsig((struct pipe *)fde->obj, end, (int)arg);
        }
        if (fde->type == FD_FILE && fde->obj) {
            ((struct vfs_file *)fde->obj)->f_sig = (int)arg;
        }
        return 0;
    case F_SETLEASE:
        return fcntl_setlease(fde, arg);
    case F_GETLEASE:
        return fcntl_getlease(fde);
    case F_NOTIFY: {
        /* dnotify: nur auf Directory-FDs zulaessig. Pfad kommt aus vfs_file. */
        if (fde->type != FD_FILE) return -ENOTDIR;
        struct vfs_file *f = (struct vfs_file *)fde->obj;
        if (!f || f->type != VFS_DIR) return -ENOTDIR;
        return dnotify_ctl(fd, f->path, (uint32_t)arg, f->f_sig);
    }
    case F_GETPIPE_SZ: {
        if (fde->type != FD_PIPE) return -EINVAL;
        struct pipe *pp = (struct pipe *)fde->obj;
        return (long)pipe_get_size(pp);
    }
    case F_SETPIPE_SZ: {
        if (fde->type != FD_PIPE) return -EINVAL;
        if ((unsigned long)arg >= (1UL << 31)) return -EINVAL;
        /* Linux fs/pipe.c:pipe_set_size: CAP_SYS_RESOURCE darf ueber
         * pipe-max-size, ohne EPERM. Aus p->euid==0 abzuleiten waere
         * falsch fuer LTP-Tests die per PR_CAPBSET_DROP die Cap entfernen. */
        int pmax = pipe_max_size_get();
        int has_cap = (p->cap_effective & CAP_TO_MASK(CAP_SYS_RESOURCE)) != 0;
        if (!has_cap && (long)arg > pmax) return -EPERM;
        struct pipe *pp = (struct pipe *)fde->obj;
        int req = (int)arg;
        if (req < PIPE_BUF_MIN) req = PIPE_BUF_MIN;
        return pipe_resize(pp, req);
    }
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        fd_entry_t src = *fde;
        int new_flags = src.flags & ~O_CLOEXEC;
        if (cmd == F_DUPFD_CLOEXEC) new_flags |= O_CLOEXEC;
        int i = fd_dup_at(p->fds, (int)arg, src, new_flags);
        if (i < 0) return i;
        if (src.type == FD_FILE && src.obj) {
            extern void vfs_file_incref(struct vfs_file *f);
            vfs_file_incref((struct vfs_file *)src.obj);
        } else if (src.obj) {
            fd_obj_incref(src.type, src.obj, src.flags);
        }
        return i;
    }
    default: return -EINVAL;
    }
}

/* ── SYS_pread64 (17) ────────────────────────────── */

long do_pread64(int fd, void *buf, size_t count, int64_t offset) {
    if (__builtin_expect(!user_ok((uint64_t)buf, count), 0)) return -EFAULT;
    if (offset < 0) return -EINVAL;
    process_t *p = proc_current();
    if (__builtin_expect(!p, 0)) return -EFAULT;
    fd_entry_t *fde = fd_get(p->fds, fd);
    if (__builtin_expect(!fde, 0)) return -EBADF;
    if (fde->type != FD_FILE) return -ESPIPE;
    if (__builtin_expect(!fde->obj, 0)) return -EBADF;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    return vfs_pread(f, buf, count, (uint64_t)offset);
}

/* ── SYS_pwrite64 (18) ───────────────────────────── */

long do_pwrite64(int fd, const void *buf, size_t count, int64_t offset) {
    if (__builtin_expect(!user_ok((uint64_t)buf, count), 0)) return -EFAULT;
    if (offset < 0) return -EINVAL;
    process_t *p = proc_current();
    if (__builtin_expect(!p, 0)) return -EFAULT;
    fd_entry_t *fde = fd_get(p->fds, fd);
    if (__builtin_expect(!fde, 0)) return -EBADF;
    if (fde->type != FD_FILE) return -ESPIPE;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    return vfs_pwrite(f, buf, count, (uint64_t)offset);
}

/* ── SYS_fchdir (81) — change CWD by fd ─────────── */

long do_fchdir(int fd) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(p->fds, fd);
    if (!fde) return -EBADF;
    if (fde->type != FD_FILE) return -ENOTDIR;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f || f->type != VFS_DIR) return -ENOTDIR;
    if (!f->path[0]) return -EBADF;
    /* Linux fs/open.c ksys_fchdir → inode_permission(MAY_EXEC). Non-root
     * ohne exec-Bit auf dem Verzeichnis bekommt EACCES. */
    if (p->euid != 0 &&
        !(p->cap_effective & (CAP_TO_MASK(CAP_DAC_OVERRIDE) |
                              CAP_TO_MASK(CAP_DAC_READ_SEARCH)))) {
        struct k_stat st;
        int rc = vfs_fstat(fd, &st);
        if (rc < 0) return rc;
        rc = cred_may_access(p, st.st_uid, st.st_gid, st.st_mode, MAY_EXEC);
        if (rc < 0) return rc;
    }
    int i = 0;
    while (f->path[i] && i < 255) { p->cwd[i] = f->path[i]; i++; }
    p->cwd[i] = '\0';
    return 0;
}

/* ── SYS_creat (85) — open(path, O_CREAT|O_WRONLY|O_TRUNC, mode) ── */

long do_creat(const char *path, int mode) {
    return do_open(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

/* ── SYS_getdents (78) — old getdents format ──────── */

/* Linux old getdents uses struct linux_dirent:
 *   d_ino (8), d_off (8), d_reclen (2), d_name[], d_type (1 byte after name+pad)
 * Same layout as getdents64 — delegate directly. The struct layouts differ
 * in theory (getdents has unsigned long d_ino, long d_off vs uint64_t in getdents64)
 * but on x86_64 they're identical in size. Real difference: d_type is the last
 * byte of the record in old getdents vs a field in getdents64.
 * Since our emit_dirent already writes getdents64 format which glibc/musl
 * handle correctly via getdents64, and callers of old getdents are rare,
 * delegate to getdents64 — the struct layouts match on x86_64. */
long do_getdents(int fd, void *buf, size_t count) {
    return do_getdents64(fd, buf, count);
}

/* ── SYS_close_range (436) — close FDs in range ──── */

#define CLOSE_RANGE_UNSHARE 2
#define CLOSE_RANGE_CLOEXEC 4
long do_close_range(unsigned int first, unsigned int last, unsigned int flags) {
    if (flags & ~(CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC)) return -EINVAL;
    if (first > last) return -EINVAL;
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    /* CLOSE_RANGE_UNSHARE: detach a private fd_table before mutating it so
     * the changes don't leak into peers that share via clone(CLONE_FILES).
     * Linux fs/file.c::__close_range. Must run before either CLOEXEC-mark
     * or the close loop because both touch entries owned by the table. */
    if (flags & CLOSE_RANGE_UNSHARE) {
        int us = fd_table_unshare(p);
        if (us < 0) return us;
    }

    int slot_max = p->fds ? p->fds->max_slots : 0;
    if (slot_max > 0 && last >= (unsigned int)slot_max) last = (unsigned int)slot_max - 1;

    if (flags & CLOSE_RANGE_CLOEXEC) {
        for (unsigned int fd = first; fd <= last; fd++) {
            fd_entry_t *fde = fd_get(p->fds, (int)fd);
            if (fde) fde->flags |= O_CLOEXEC;
        }
        return 0;
    }
    for (unsigned int fd = first; fd <= last; fd++)
        do_close((int)fd);
    return 0;
}

/* ── SYS_copy_file_range (326) — read+write between fds ── */

long do_copy_file_range(int fd_in, long *off_in, int fd_out, long *off_out,
                        size_t len, unsigned int flags) {
    (void)flags;
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde_in = fd_get(p->fds, fd_in);
    fd_entry_t *fde_out = fd_get(p->fds, fd_out);
    if (!fde_in || !fde_out) return -EBADF;
    if (fde_in->type != FD_FILE || fde_out->type != FD_FILE) return -EINVAL;
    struct vfs_file *fin = (struct vfs_file *)fde_in->obj;
    struct vfs_file *fout = (struct vfs_file *)fde_out->obj;
    if (!fin || !fout) return -EBADF;

    uint64_t pos_in = off_in ? (uint64_t)*off_in : fin->offset;
    uint64_t pos_out = off_out ? (uint64_t)*off_out : fout->offset;

    uint8_t kbuf[4096];
    size_t total = 0;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        long nr = vfs_pread(fin, kbuf, chunk, pos_in);
        if (nr <= 0) break;
        long nw = vfs_pwrite(fout, kbuf, (size_t)nr, pos_out);
        if (nw <= 0) break;
        pos_in += (uint64_t)nw;
        pos_out += (uint64_t)nw;
        total += (size_t)nw;
        if (nw < nr) break;
    }

    if (off_in) *off_in = (long)pos_in;
    else fin->offset = pos_in;
    if (off_out) *off_out = (long)pos_out;
    else fout->offset = pos_out;

    return total ? (long)total : -EIO;
}

/* ── SYS_fadvise64 (221) — file access pattern hint ──── */

long do_fadvise64(int fd, long offset, long len, int advice) {
    (void)offset; (void)len;
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(p->fds, fd);
    if (!fde || fde->type == FD_NONE) return -EBADF;
    if (advice < POSIX_FADV_NORMAL || advice > POSIX_FADV_NOREUSE) return -EINVAL;
    if (fde->type == FD_PIPE || fde->type == FD_SOCKET ||
        fde->type == FD_UNIX_SOCK) return -ESPIPE;
    return 0; /* hint accepted, no page cache action */
}

/* ── SYS_memfd_create (319) — anonymous file in memory ── */

long do_memfd_create(const char *uname, unsigned int flags) {
    (void)uname; (void)flags;
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    /* Create an anonymous ramfs file under /dev/shm/memfd_<pid>_<counter> */
    static int memfd_counter;
    int id = __sync_fetch_and_add(&memfd_counter, 1);

    char path[64];
    char *w = path;
    const char *prefix = "/dev/shm/memfd:";
    while (*prefix) *w++ = *prefix++;
    /* Append counter */
    { int v = id; char t[12]; int ti = 0;
      do { t[ti++] = '0' + (char)(v % 10); v /= 10; } while (v);
      while (ti--) *w++ = t[ti]; }
    *w = '\0';

    /* Create backing file in ramfs */
    struct vfs_node *node = vfs_create(path, VFS_FILE);
    if (!node) return -ENOMEM;

    /* Allocate vfs_file + fd */
    extern struct vfs_file *file_alloc(void);
    struct vfs_file *f = file_alloc();
    if (!f) return -ENOMEM;
    f->type = VFS_FILE;
    f->flags = O_RDWR;
    f->refcount = 1;
    f->backend = VFS_BACKEND_RAM;
    f->offset = 0;
    f->inode = node->inode;
    { int i = 0; while (path[i] && i < 255) { f->path[i] = path[i]; i++; } f->path[i] = '\0'; }

    int fd = fd_alloc(p->fds, FD_FILE, f, O_RDWR);
    if (fd < 0) { vfs_file_free_obj(f); return -EMFILE; }
    return fd;
}
