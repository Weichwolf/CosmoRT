/* CosmoRT Syscall Layer — file I/O syscalls */

#include "internal.h"
#include "core/event_queue.h"

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

    /* Normalize: collapse "//" , resolve "/." and "/.." in-place */
    char *w = out, *r = out;
    if (*r == '/') *w++ = *r++;
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
    fd_entry_t *fde = fd_get(&p->fds, dirfd);
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
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (__builtin_expect(!fde, 0)) return -EBADF;
    if (__builtin_expect((fde->flags & O_ACCMODE) == O_RDONLY, 0)) return -EBADF;
    if (fde->type == FD_DEVICE) {
        int devid = (int)(uintptr_t)fde->obj;
        if (devid == DEV_NULL || devid == DEV_ZERO || devid == DEV_URANDOM)
            return (long)count; /* discard */
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
        return eventfd_write(fde->obj, buf, (long)count);
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
        fd_entry_t *fde = fd_get(&p->fds, fd);
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
    fd_entry_t *fde = fd_get(&p->fds, fd);
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
        return eventfd_read(fde->obj, buf, (long)count);
    if (fde->type == FD_TIMERFD)
        return timerfd_read(fde->obj, buf, (long)count);
    if (fde->type == FD_INOTIFY)
        return inotify_read(fde->obj, buf, (long)count);
    if (fde->type == FD_PTY_SLAVE) {
        int pty_id = (int)(long)fde->obj;
        uint8_t kbuf[256];
        size_t want = count > 256 ? 256 : count;
        for (;;) {
            int got = pty_slave_read(pty_id, (char *)kbuf, (int)want);
            if (got > 0) {
                copy_to_user(buf, kbuf, (size_t)got);
                extern void vt_flush(int vt_id);
                vt_flush(pty_id);
                return (long)got;
            }
            /* No data — block until pty_master_write event_posts us */
            thread_t *t = thread_current();
            pty_t *pty = pty_get(pty_id);
            if (!t || !pty) return -EAGAIN;

            uint64_t irqf;
            spin_lock_irq(&pty->lock, &irqf);
            int avail = (pty->input_tail - pty->input_head
                         + PTY_BUF_SIZE) % PTY_BUF_SIZE;
            if (avail > 0) {
                int n = avail > (int)want ? (int)want : avail;
                for (int i = 0; i < n; i++) {
                    kbuf[i] = (uint8_t)pty->input_buf[pty->input_head];
                    pty->input_head = (pty->input_head + 1) % PTY_BUF_SIZE;
                }
                spin_unlock_irq(&pty->lock, irqf);
                copy_to_user(buf, kbuf, (size_t)n);
                extern void vt_flush(int vt_id);
                vt_flush(pty_id);
                return (long)n;
            }
            pty->blocked_reader = t;
            spin_unlock_irq(&pty->lock, irqf);

            extern void vt_flush(int vt_id);
            vt_flush(pty_id);

            /* Check for pending signals before blocking (POSIX: blocking
             * read must return -EINTR when a signal is deliverable).
             * Without this, signals like SIGCHLD stay pending while the
             * thread keeps re-blocking in event_wait. */
            if (t->proc) {
                uint64_t deliverable = t->proc->sig_pending & ~t->sig_blocked;
                if (deliverable) return -EINTR;
            }

            event_t ev;
            int _wr = event_wait(&t->eq, &ev, -1);
            if (_wr == -4) return -EINTR;
            /* If returned, loop re-checks. */
        }
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
        fd_entry_t *fde = fd_get(&p->fds, fd);
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
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;
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
        return fd_close(&p->fds, fd);
    }
    if (fde->type == FD_SOCKET)
        return socket_close(fd);
    if (fde->type == FD_UNIX_SOCK)
        return usock_close(fd);
    if (fde->type == FD_PIPE) {
        long r = pipe_close(fde);
        fd_close(&p->fds, fd);
        return r;
    }
    if (fde->type == FD_EPOLL)   { epoll_destroy(fde->obj);   return fd_close(&p->fds, fd); }
    if (fde->type == FD_EVENTFD) { eventfd_destroy(fde->obj); return fd_close(&p->fds, fd); }
    if (fde->type == FD_TIMERFD) { timerfd_destroy(fde->obj); return fd_close(&p->fds, fd); }
    if (fde->type == FD_INOTIFY) { inotify_destroy(fde->obj); return fd_close(&p->fds, fd); }
    if (fde->type == FD_PTY_MASTER || fde->type == FD_PTY_SLAVE)
        return fd_close(&p->fds, fd);
    return fd_close(&p->fds, fd);
}

/* ── SYS_openat (257) — primary; SYS_open delegates with AT_FDCWD ── */

long do_openat(int dirfd, const char *path, int flags, int mode) {
    char kpath[PATH_MAX], rpath[PATH_MAX];
    int len = resolve_at_path(dirfd, path, kpath, PATH_MAX);
    if (len < 0) return len;
    resolve_path(kpath, rpath, PATH_MAX);
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
    if (oldfd < 0 || oldfd >= FD_MAX || newfd < 0 || newfd >= FD_MAX) return -EBADF;
    fd_entry_t *old = fd_get(&p->fds, oldfd);
    if (!old) return -EBADF;

    /* Close newfd if open (must match do_close logic) */
    fd_entry_t *cur = fd_get(&p->fds, newfd);
    if (cur) {
        if (cur->type == FD_FILE) {
            uint64_t ino = flock_ino(cur);
            if (ino) flock_release(ino, p->pid);
            vfs_close(newfd);
        } else if (cur->type == FD_PIPE) pipe_close(cur);
        else {
            fd_cleanup_entry(cur->type, cur->obj);
            fd_close(&p->fds, newfd);
        }
        p->fds.entries[newfd].type = FD_NONE;
        p->fds.entries[newfd].obj = 0;
    }

    /* Copy the fd entry and bump refcount.
     * dup2 clears O_CLOEXEC on the new fd (POSIX). dup3 sets it only if
     * O_CLOEXEC is in flags. */
    p->fds.entries[newfd] = *old;
    p->fds.entries[newfd].flags &= ~O_CLOEXEC;  /* dup2: always clear */
    fd_mark_used(&p->fds, newfd);
    if (old->type == FD_FILE && old->obj) {
        extern void vfs_file_incref(struct vfs_file *f);
        vfs_file_incref((struct vfs_file *)old->obj);
    } else if (old->obj) {
        fd_obj_incref(old->type, old->obj);
    }
    if (flags & O_CLOEXEC)
        p->fds.entries[newfd].flags |= O_CLOEXEC;
    if (newfd >= p->fds.max_fd) p->fds.max_fd = newfd + 1;
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
    fd_entry_t *fde = fd_get(&p->fds, fd);
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
            for (int i = pf->offset; i < FD_MAX; i++) {
                fd_entry_t *e = fd_get(&p->fds, i);
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
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;

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

/* ── POSIX Advisory File Locking ─────────────────── */

#define FLOCK_MAX 64

/* whole-file flock(2) marker distinguishes flock from fcntl byte-range locks.
 * flock locks conflict across fds but not across owners of the same fd-chain. */
#define FLOCK_TYPE_FLOCK_SH  (F_RDLCK | 0x10)
#define FLOCK_TYPE_FLOCK_EX  (F_WRLCK | 0x10)

struct flock_entry {
    uint64_t ino;       /* file identity (node ptr or disk_ino), 0 = free */
    uint32_t pid;       /* lock owner (fcntl locks) */
    uint64_t owner;     /* flock(2): vfs_file *; fcntl: pid (broadcasted) */
    short    type;      /* F_RDLCK or F_WRLCK (| 0x10 for flock(2)) */
    long     start;
    long     end;       /* inclusive end; OFF_MAX = to EOF */
};

#define FLOCK_OFF_MAX __LONG_MAX__

static struct flock_entry flock_table[FLOCK_MAX];

/* Get a stable inode identity from an fd entry (must be FD_FILE) */
static uint64_t flock_ino(fd_entry_t *fde) {
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return 0;
    if (f->disk_ino) return f->disk_ino;
    return (uint64_t)(uintptr_t)f->inode;
}

static short flock_base_type(short t) { return (short)(t & ~0x10); }

static int flock_range_overlap(long s1, long e1, long s2, long e2) {
    return s1 <= e2 && s2 <= e1;
}

static int flock_is_fcntl(short t) { return !(t & 0x10); }

/* Check for fcntl byte-range conflict (F_RDLCK/F_WRLCK vs same) */
static struct flock_entry *flock_fcntl_conflict(uint64_t ino, uint32_t pid,
                                                 short type, long start, long end) {
    for (int i = 0; i < FLOCK_MAX; i++) {
        struct flock_entry *e = &flock_table[i];
        if (!e->ino || e->ino != ino) continue;
        if (!flock_is_fcntl(e->type)) continue;
        if (e->pid == pid) continue;
        if (!flock_range_overlap(e->start, e->end, start, end)) continue;
        if (e->type == F_RDLCK && type == F_RDLCK) continue;
        return e;
    }
    return (void *)0;
}

/* Allocate a new lock slot */
static int flock_alloc_slot(void) {
    for (int i = 0; i < FLOCK_MAX; i++)
        if (!flock_table[i].ino) return i;
    return -1;
}

/* Remove/split existing fcntl locks by this pid overlapping [start,end].
 * Linux semantics: overlapping same-pid locks are consumed (unlocked range
 * before re-lock). Returns 0 on success, -ENOLCK on split-split-OOM. */
static int flock_remove_overlapping(uint64_t ino, uint32_t pid,
                                     long start, long end) {
    for (int i = 0; i < FLOCK_MAX; i++) {
        struct flock_entry *e = &flock_table[i];
        if (!e->ino || e->ino != ino || e->pid != pid) continue;
        if (!flock_is_fcntl(e->type)) continue;
        if (!flock_range_overlap(e->start, e->end, start, end)) continue;

        long es = e->start, ee = e->end;
        short et = e->type;
        if (es < start && ee > end) {
            /* split into two: [es, start-1] and [end+1, ee] */
            e->end = start - 1;
            int ns = flock_alloc_slot();
            if (ns < 0) return -ENOLCK;
            flock_table[ns] = (struct flock_entry){
                .ino = ino, .pid = pid, .type = et,
                .start = end + 1, .end = ee
            };
        } else if (es < start) {
            e->end = start - 1;
        } else if (ee > end) {
            e->start = end + 1;
        } else {
            e->ino = 0;
        }
    }
    return 0;
}

/* Merge adjacent/overlapping same-type locks by same pid (linux sematics) */
static void flock_merge(uint64_t ino, uint32_t pid, short type) {
    for (int i = 0; i < FLOCK_MAX; i++) {
        struct flock_entry *a = &flock_table[i];
        if (!a->ino || a->ino != ino || a->pid != pid || a->type != type) continue;
        for (int j = i + 1; j < FLOCK_MAX; j++) {
            struct flock_entry *b = &flock_table[j];
            if (!b->ino || b->ino != ino || b->pid != pid || b->type != type) continue;
            if (a->end + 1 < b->start || b->end + 1 < a->start) continue;
            long s = a->start < b->start ? a->start : b->start;
            long e = a->end > b->end ? a->end : b->end;
            a->start = s;
            a->end = e;
            b->ino = 0;
        }
    }
}

static long flock_fcntl_setlk(uint64_t ino, uint32_t pid,
                               short type, long start, long end) {
    if (type == F_UNLCK)
        return flock_remove_overlapping(ino, pid, start, end);
    if (flock_fcntl_conflict(ino, pid, type, start, end))
        return -EAGAIN;
    int r = flock_remove_overlapping(ino, pid, start, end);
    if (r) return r;
    int ns = flock_alloc_slot();
    if (ns < 0) return -ENOLCK;
    flock_table[ns] = (struct flock_entry){
        .ino = ino, .pid = pid, .type = type, .start = start, .end = end
    };
    flock_merge(ino, pid, type);
    return 0;
}

/* flock(2): whole-file lock, keyed by (inode, owner = vfs_file*). Distinct
 * open(2) file descriptions of the same process conflict — see flock(2). */
static struct flock_entry *flock_whole_conflict(uint64_t ino, uint64_t owner,
                                                 short type) {
    for (int i = 0; i < FLOCK_MAX; i++) {
        struct flock_entry *e = &flock_table[i];
        if (!e->ino || e->ino != ino) continue;
        if (flock_is_fcntl(e->type)) continue;
        if (e->owner == owner) continue;
        if (e->type == FLOCK_TYPE_FLOCK_SH && type == FLOCK_TYPE_FLOCK_SH) continue;
        return e;
    }
    return (void *)0;
}

static long flock_whole_setlk(uint64_t ino, uint32_t pid, uint64_t owner,
                               short type) {
    if (flock_whole_conflict(ino, owner, type))
        return -EAGAIN;
    /* Upgrade/downgrade: replace this owner's existing whole-file lock */
    for (int i = 0; i < FLOCK_MAX; i++) {
        struct flock_entry *e = &flock_table[i];
        if (e->ino == ino && e->owner == owner && !flock_is_fcntl(e->type)) {
            e->type = type;
            return 0;
        }
    }
    int ns = flock_alloc_slot();
    if (ns < 0) return -ENOLCK;
    flock_table[ns] = (struct flock_entry){
        .ino = ino, .pid = pid, .owner = owner, .type = type,
        .start = 0, .end = FLOCK_OFF_MAX
    };
    return 0;
}

static void flock_whole_unlock(uint64_t ino, uint64_t owner) {
    for (int i = 0; i < FLOCK_MAX; i++) {
        struct flock_entry *e = &flock_table[i];
        if (e->ino == ino && e->owner == owner && !flock_is_fcntl(e->type))
            e->ino = 0;
    }
}

/* Test-only helper: kept under old name for F_GETLK (byte-range) */
static struct flock_entry *flock_conflict(uint64_t ino, uint32_t pid,
                                           short type, long start, long end) {
    return flock_fcntl_conflict(ino, pid, type, start, end);
}

/* Remove all locks held by pid on a given inode (called on close).
 * POSIX: close() releases all fcntl-style locks held by the calling process
 * on the underlying file. flock() owner identity is the struct file, so
 * dup'd fds share flock ownership; whole-file locks persist while any
 * fd from the same open() is open. refcount handling happens via
 * vfs_file lifecycle — flock_release_file() is the proper flock-path. */
void flock_release(uint64_t ino, uint32_t pid) {
    for (int i = 0; i < FLOCK_MAX; i++) {
        struct flock_entry *e = &flock_table[i];
        if (e->ino == ino && e->pid == pid && flock_is_fcntl(e->type))
            e->ino = 0;
    }
}

/* Release flock(2) locks when a struct vfs_file is finally freed */
void flock_release_file(void *vfs_file_ptr) {
    uint64_t owner = (uint64_t)(uintptr_t)vfs_file_ptr;
    for (int i = 0; i < FLOCK_MAX; i++) {
        struct flock_entry *e = &flock_table[i];
        if (e->owner == owner && !flock_is_fcntl(e->type))
            e->ino = 0;
    }
}

/* Remove all locks held by a process (called on exit) */
void flock_release_pid(uint32_t pid) {
    for (int i = 0; i < FLOCK_MAX; i++)
        if (flock_table[i].pid == pid)
            flock_table[i].ino = 0;
}

/* ── SYS_flock (73) — whole-file advisory locking ── */

long do_flock(int fd, int operation) {
    int nb = (operation & LOCK_NB) ? 1 : 0;
    int op = operation & ~LOCK_NB;

    if (op != LOCK_SH && op != LOCK_EX && op != LOCK_UN) return -EINVAL;
    /* LOCK_NB alone (without SH/EX/UN) is invalid */
    if (operation == LOCK_NB) return -EINVAL;

    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type == FD_NONE) return -EBADF;
    if (fde->type != FD_FILE) return -EINVAL;
    uint64_t ino = flock_ino(fde);
    if (!ino) return -EBADF;

    uint64_t owner = (uint64_t)(uintptr_t)fde->obj;

    if (op == LOCK_UN) {
        flock_whole_unlock(ino, owner);
        return 0;
    }

    short t = (op == LOCK_SH) ? FLOCK_TYPE_FLOCK_SH : FLOCK_TYPE_FLOCK_EX;
    long r = flock_whole_setlk(ino, p->pid, owner, t);
    if (r == -EAGAIN && nb) return -EAGAIN;
    if (r != -EAGAIN) return r;

    /* Blocking: poll-loop with signal wakeup */
    while (1) {
        thread_t *th = thread_current();
        if (th && th->proc) {
            uint64_t deliverable = (th->proc->sig_pending | th->sig_thread_pending) & ~th->sig_blocked;
            if (deliverable) return -EINTR;
        }
        thread_block_ms(10);
        r = flock_whole_setlk(ino, p->pid, owner, t);
        if (r != -EAGAIN) return r;
    }
}

long do_fcntl(int fd, int cmd, long arg) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;

    switch (cmd) {
    case F_GETFL: return fde->flags & ~O_CLOEXEC; /* CLOEXEC is fd-flag, not file-flag */
    case F_SETFL: {
        /* Only O_APPEND and O_NONBLOCK are settable via F_SETFL.
         * Preserve access mode (O_RDONLY/O_WRONLY/O_RDWR) and O_CLOEXEC. */
        int keep = fde->flags & (O_RDONLY | O_WRONLY | O_RDWR | O_CLOEXEC);
        fde->flags = keep | ((int)arg & (O_APPEND | O_NONBLOCK));
        return 0;
    }
    case F_GETFD: return (fde->flags & O_CLOEXEC) ? 1 : 0;
    case F_SETFD: {
        if (arg & 1) fde->flags |= O_CLOEXEC;
        else fde->flags &= ~O_CLOEXEC;
        return 0;
    }
    case F_GETLK: {
        struct k_flock *fl = (struct k_flock *)arg;
        if (!user_ok((uint64_t)fl, sizeof(*fl))) return -EFAULT;
        if (fl->l_type != F_RDLCK && fl->l_type != F_WRLCK) return -EINVAL;
        if (fde->type != FD_FILE) return -EBADF;
        uint64_t ino = flock_ino(fde);
        if (!ino) { fl->l_type = F_UNLCK; return 0; }
        long qstart = fl->l_start;
        long qend = fl->l_len ? qstart + fl->l_len - 1 : FLOCK_OFF_MAX;
        struct flock_entry *c = flock_conflict(ino, p->pid,
                                                fl->l_type, qstart, qend);
        if (c) {
            fl->l_type   = flock_base_type(c->type);
            fl->l_whence = 0; /* SEEK_SET */
            fl->l_start  = c->start;
            fl->l_len    = (c->end == FLOCK_OFF_MAX) ? 0 : (c->end - c->start + 1);
            fl->l_pid    = (int)c->pid;
        } else {
            fl->l_type = F_UNLCK;
        }
        return 0;
    }
    case F_SETLK:
    case F_SETLKW: {
        struct k_flock *fl = (struct k_flock *)arg;
        if (!user_ok((uint64_t)fl, sizeof(*fl))) return -EFAULT;
        if (fl->l_type != F_RDLCK && fl->l_type != F_WRLCK && fl->l_type != F_UNLCK)
            return -EINVAL;
        /* Pipes, sockets, and other non-seekable fds: EINVAL for F_SETLK */
        if (fde->type != FD_FILE) return -EINVAL;
        uint64_t ino = flock_ino(fde);
        if (!ino) return -EBADF;
        long start = fl->l_start;
        long end = fl->l_len ? start + fl->l_len - 1 : FLOCK_OFF_MAX;
        if (start < 0) return -EINVAL;
        long r = flock_fcntl_setlk(ino, p->pid, fl->l_type, start, end);
        if (cmd == F_SETLK) return r;
        /* F_SETLKW: block until lock obtainable or signal */
        while (r == -EAGAIN) {
            thread_t *th = thread_current();
            if (th && th->proc) {
                uint64_t deliverable = (th->proc->sig_pending | th->sig_thread_pending) & ~th->sig_blocked;
                if (deliverable) return -EINTR;
            }
            thread_block_ms(10);
            r = flock_fcntl_setlk(ino, p->pid, fl->l_type, start, end);
        }
        return r;
    }
    case F_GETOWN:
        return 0; /* no SIGIO support — always returns 0 */
    case F_SETOWN:
        return 0; /* no-op: no SIGIO support */
    case F_GETPIPE_SZ:
        return 65536; /* default pipe buffer size */
    case F_SETPIPE_SZ:
        return 65536; /* accept but return fixed size */
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        int i = fd_find_free(&p->fds, (int)arg);
        if (i < 0) return -EMFILE;
        p->fds.entries[i] = *fde;
        p->fds.entries[i].flags &= ~O_CLOEXEC; /* F_DUPFD: always clear */
        fd_mark_used(&p->fds, i);
        if (cmd == F_DUPFD_CLOEXEC)
            p->fds.entries[i].flags |= O_CLOEXEC;
        if (i >= p->fds.max_fd) p->fds.max_fd = i + 1;
        /* Increment refcount for vfs_file if needed */
        if (fde->type == FD_FILE && fde->obj) {
            extern void vfs_file_incref(struct vfs_file *f);
            vfs_file_incref((struct vfs_file *)fde->obj);
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
    fd_entry_t *fde = fd_get(&p->fds, fd);
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
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (__builtin_expect(!fde, 0)) return -EBADF;
    if (fde->type != FD_FILE) return -ESPIPE;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    return vfs_pwrite(f, buf, count, (uint64_t)offset);
}

/* ── SYS_fchdir (81) — change CWD by fd ─────────── */

long do_fchdir(int fd) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;
    if (fde->type != FD_FILE) return -ENOTDIR;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f || f->type != VFS_DIR) return -ENOTDIR;
    if (!f->path[0]) return -EBADF;
    /* Copy path to process CWD */
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
    if (last >= FD_MAX) last = FD_MAX - 1;
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
    fd_entry_t *fde_in = fd_get(&p->fds, fd_in);
    fd_entry_t *fde_out = fd_get(&p->fds, fd_out);
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
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type == FD_NONE) return -EBADF;
    if (advice < 0 || advice > 5) return -EINVAL;
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

    int fd = fd_alloc(&p->fds, FD_FILE, f, O_RDWR);
    if (fd < 0) { vfs_file_free_obj(f); return -EMFILE; }
    return fd;
}
