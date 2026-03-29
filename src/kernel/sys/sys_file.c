/* CosmoRT Syscall Layer — file I/O syscalls */

#include "internal.h"
#include "core/event_queue.h"

/* Resolve a relative path against CWD, handling "." and ".." components.
 * Result written to out (max outsize bytes). Returns 0 on success. */
int resolve_path(const char *path, char *out, int outsize) {
    if (!path || !out || outsize < 2) return -EINVAL;

    /* Start from CWD for relative paths */
    int oi = 0;
    if (path[0] != '/') {
        process_t *p = proc_current();
        const char *cwd = p ? p->cwd : "/";
        while (*cwd && oi < outsize - 1) out[oi++] = *cwd++;
        if (oi > 1 && out[oi - 1] != '/' && oi < outsize - 1) out[oi++] = '/';
    }
    /* Append path */
    while (*path && oi < outsize - 1) out[oi++] = *path++;
    out[oi] = '\0';

    /* Normalize: resolve "." and ".." in-place */
    char *w = out, *r = out;
    if (*r == '/') *w++ = *r++;
    while (*r) {
        if (r[0] == '/' && r[1] == '.' && (r[2] == '/' || r[2] == '\0')) {
            r += 2; /* skip "/." */
        } else if (r[0] == '/' && r[1] == '.' && r[2] == '.' && (r[3] == '/' || r[3] == '\0')) {
            r += 3; /* skip "/.." */
            if (w > out + 1) { w--; while (w > out + 1 && w[-1] != '/') w--; }
        } else {
            *w++ = *r++;
        }
    }
    if (w == out) *w++ = '/'; /* root */
    /* Remove trailing slash (unless root) */
    if (w > out + 1 && w[-1] == '/') w--;
    *w = '\0';
    return 0;
}

/* Resolve dirfd + relative path.
 * Absolute paths and AT_FDCWD are handled directly.
 * Real dirfd with relative path → prepend directory path from open fd. */
int resolve_at_path(int dirfd, const char *upath, char *kpath, int max) {
    int len = copy_path_from_user(kpath, upath, (size_t)max);
    if (len < 0) return len;
    if (kpath[0] == '/' || dirfd == AT_FDCWD) return len;

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

/* ── SYS_writev (20) ────────────────────────────── */

long do_writev(int fd, const struct iovec *iov, int iovcnt) {
    if (iovcnt < 0 || iovcnt > 16) return -EINVAL;
    /* Copy iov array to kernel stack to prevent TOCTOU */
    struct iovec k_iov[16];
    { int r = copy_from_user(k_iov, iov, (size_t)iovcnt * sizeof(struct iovec)); if (r) return r; }
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

/* ── SYS_read (0) ────────────────────────────────── */

__attribute__((hot))
long do_read(int fd, void *buf, size_t count) {
    if (__builtin_expect(!user_ok((uint64_t)buf, count), 0)) return -EFAULT;
    process_t *p = proc_current();
    if (__builtin_expect(!p, 0)) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (__builtin_expect(!fde, 0)) return -EBADF;
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
            event_wait(&t->eq, &ev, -1);
            /* If blocked, syscall restarts. If returned, loop re-checks. */
        }
    }
    return -EBADF;
}

/* ── SYS_readv (19) ──────────────────────────────── */

long do_readv(int fd, const struct iovec *iov, int iovcnt) {
    if (iovcnt < 0 || iovcnt > 1024) return -EINVAL;
    if (iovcnt > 64) return -EINVAL; /* kernel stack limit */
    /* Copy iovec array to kernel stack to prevent TOCTOU on iov_base/iov_len.
     * Buffer contents are still user memory — do_read validates via user_ok. */
    struct iovec kiov[64];
    { int r = copy_from_user(kiov, iov, (size_t)iovcnt * sizeof(struct iovec)); if (r) return r; }
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

/* ── SYS_close (3) ───────────────────────────────── */

long do_close(int fd) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;
    if (fde->type == FD_FILE)
        return vfs_close(fd);
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

/* ── SYS_open (2) / SYS_openat (257) ────────────────── */

long do_open(const char *path, int flags, int mode) {
    char kpath[PATH_MAX];
    int len = copy_path_from_user(kpath, path, PATH_MAX);
    if (len < 0) return len;
    return vfs_open(kpath, flags, mode);
}

long do_openat(int dirfd, const char *path, int flags, int mode) {
    char kpath[PATH_MAX];
    int len = resolve_at_path(dirfd, path, kpath, PATH_MAX);
    if (len < 0) return len;
    return vfs_open(kpath, flags, mode);
}

/* ── SYS_lseek (8) ──────────────────────────────── */

long do_lseek(int fd, long offset, int whence) {
    return vfs_lseek(fd, offset, whence);
}

/* ── SYS_dup3 (292) — primary; dup2 delegates here via dispatch ── */

long do_dup3(int oldfd, int newfd, int flags) {
    if (oldfd == newfd) return -EINVAL;
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    if (oldfd < 0 || oldfd >= FD_MAX || newfd < 0 || newfd >= FD_MAX) return -EBADF;
    fd_entry_t *old = fd_get(&p->fds, oldfd);
    if (!old) return -EBADF;

    /* Close newfd if open (must match do_close logic) */
    fd_entry_t *cur = fd_get(&p->fds, newfd);
    if (cur) {
        if (cur->type == FD_FILE) vfs_close(newfd);
        else if (cur->type == FD_PIPE) pipe_close(cur);
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
    char kpath[PATH_MAX];
    int len = copy_path_from_user(kpath, path, PATH_MAX);
    if (len < 0) return len;
    return vfs_chdir(kpath);
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

/* Callback context for ext2 getdents64 via ext2_dir_iterate */
struct getdents_ctx {
    uint8_t *out;
    size_t   count;
    size_t   written;
    uint64_t next_off;
    int      full;       /* set when buffer is exhausted */
};

static int getdents_cb(const char *name, uint32_t ino, uint8_t file_type, void *arg) {
    struct getdents_ctx *ctx = (struct getdents_ctx *)arg;
    /* Map ext2 file_type to DT_* */
    uint8_t d_type = 0; /* DT_UNKNOWN */
    if (file_type == EXT2_FT_REG_FILE) d_type = 8; /* DT_REG */
    else if (file_type == EXT2_FT_DIR) d_type = 4; /* DT_DIR */
    else if (file_type == EXT2_FT_SYMLINK) d_type = 10; /* DT_LNK */
    else d_type = 8; /* default to DT_REG */

    uint64_t new_off = ctx->next_off + 1;
    size_t n = emit_dirent(ctx->out + ctx->written, ctx->count - ctx->written,
                           ino, new_off, d_type, name);
    if (n == 0) { ctx->full = 1; return 1; /* stop — don't advance offset */ }
    ctx->next_off = new_off;
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

    /* ext2 directory */
    if (f->backend == VFS_BACKEND_EXT2) {
        if (f->type != VFS_DIR) return -ENOTDIR;
        struct getdents_ctx ctx = {
            .out = (uint8_t *)buf,
            .count = count,
            .written = 0,
            .next_off = f->offset,
            .full = 0
        };
        ext2_dir_iterate((uint32_t)f->disk_ino, (int)f->offset, getdents_cb, &ctx);
        f->offset = ctx.next_off;
        return (long)ctx.written;
    }

    /* ramfs directory */
    if (!f->node || f->node->type != VFS_DIR) return -ENOTDIR;

    struct vfs_node *dir = f->node;
    uint8_t *out = (uint8_t *)buf;
    size_t written = 0;

    /* Walk to the child at offset f->offset */
    struct vfs_node *child = dir->children;
    uint64_t idx = 0;
    while (child && idx < f->offset) {
        child = child->next;
        idx++;
    }

    while (child) {
        uint8_t d_type = (child->type == VFS_DIR) ? 4 : 8;
        size_t n = emit_dirent(out + written, count - written,
                               child->ino, f->offset + 1, d_type, child->name);
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
        /* Single-user system: no contention possible. Report unlocked. */
        struct k_flock *fl = (struct k_flock *)arg;
        if (!user_ok((uint64_t)fl, sizeof(*fl))) return -EFAULT;
        fl->l_type = F_UNLCK;
        return 0;
    }
    case F_SETLK:
    case F_SETLKW:
        /* Single-user system: advisory locks always succeed */
        if (!user_ok((uint64_t)arg, sizeof(struct k_flock))) return -EFAULT;
        return 0;
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

long do_close_range(unsigned int first, unsigned int last, unsigned int flags) {
    (void)flags;
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
    f->node = node;
    { int i = 0; while (path[i] && i < 255) { f->path[i] = path[i]; i++; } f->path[i] = '\0'; }

    int fd = fd_alloc(&p->fds, FD_FILE, f, O_RDWR);
    if (fd < 0) { vfs_file_free_obj(f); return -EMFILE; }
    return fd;
}
