/* CosmoRT Syscall Layer — file I/O syscalls */

#include "internal.h"
#include "core/event_queue.h"

int resolve_path(const char *path, char *out, int outsize) {
    if (!path || !out || outsize < 2) return -EINVAL;

    int oi = 0;
    if (path[0] != '/') {
        process_t *p = proc_current();
        const char *cwd = p ? p->cwd : "/";
        while (*cwd && oi < outsize - 1) out[oi++] = *cwd++;
        if (oi > 1 && out[oi - 1] != '/' && oi < outsize - 1) out[oi++] = '/';
    }
    while (*path && oi < outsize - 1) out[oi++] = *path++;
    out[oi] = '\0';

    char *w = out, *r = out;
    if (*r == '/') *w++ = *r++;
    while (*r) {
        if (r[0] == '/' && r[1] == '.' && (r[2] == '/' || r[2] == '\0')) {
            r += 2;
        } else if (r[0] == '/' && r[1] == '.' && r[2] == '.' && (r[3] == '/' || r[3] == '\0')) {
            r += 3;
            if (w > out + 1) { w--; while (w > out + 1 && w[-1] != '/') w--; }
        } else {
            *w++ = *r++;
        }
    }
    if (w == out) *w++ = '/';
    if (w > out + 1 && w[-1] == '/') w--;
    *w = '\0';
    return 0;
}

int resolve_at_path(int dirfd, const char *upath, char *kpath, int max) {
    int len = copy_path_from_user(kpath, upath, (size_t)max);
    if (len < 0) return len;
    if (kpath[0] == '/') return len;

    if (dirfd == AT_FDCWD) {
        char tmp[PATH_MAX];
        for (int i = 0; i <= len && i < PATH_MAX; i++) tmp[i] = kpath[i];
        return resolve_path(tmp, kpath, max);
    }

    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, dirfd);
    if (!fde) return -EBADF;
    if (fde->type != FD_FILE) return -ENOTDIR;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f || f->type != VFS_DIR) return -ENOTDIR;
    if (!f->path[0]) return -EBADF;

    char tmp[PATH_MAX];
    int di = 0;
    const char *dp = f->path;
    while (*dp && di < PATH_MAX - 2) tmp[di++] = *dp++;
    if (di > 0 && tmp[di - 1] != '/') tmp[di++] = '/';
    const char *rp = kpath;
    while (*rp && di < PATH_MAX - 1) tmp[di++] = *rp++;
    tmp[di] = '\0';

    return resolve_path(tmp, kpath, max);
}

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
            return (long)count;
        if (devid == DEV_TTY)  {
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
        if (!pf) return -EBADF;
        extern int procfs_write(int handle, const char *buf, int len);
        extern int procfs_pid_write(const char *name, const char *buf, int len);
        char kbuf[256];
        int want = (int)count;
        if (want > (int)sizeof(kbuf)) want = (int)sizeof(kbuf);
        copy_from_user(kbuf, buf, (size_t)want);
        int r;
        if (pf->handle == -2)
            r = procfs_pid_write(pf->name, kbuf, want);
        else
            r = procfs_write(pf->handle, kbuf, want);
        return (long)r;
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
        vt_flush(pty_id);
        return (long)pos;
    }
    return -EBADF;
}

long do_writev(int fd, const struct iovec *iov, int iovcnt) {
    if (iovcnt < 0 || iovcnt > 16) return -EINVAL;
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

__attribute__((hot))
long do_read(int fd, void *buf, size_t count) {
    if (__builtin_expect(!user_ok((uint64_t)buf, count), 0)) return -EFAULT;
    process_t *p = proc_current();
    if (__builtin_expect(!p, 0)) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (__builtin_expect(!fde, 0)) return -EBADF;
    if (fde->type == FD_DEVICE) {
        int devid = (int)(uintptr_t)fde->obj;
        if (devid == DEV_NULL)    return 0;
        if (devid == DEV_ZERO) {
            size_t actual = count > 0x10000 ? 0x10000 : count;
            kmemset(buf, 0, actual);
            return (long)actual;
        }
        if (devid == DEV_URANDOM) {
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
        extern long usock_read_blocking(unix_socket_t *s, void *buf, long count);
        unix_socket_t *s = usock_from_fd(fd);
        if (!s) return -EBADF;
        return usock_read_blocking(s, buf, (long)count);
    }
    if (fde->type == FD_PIPE) {
        int is_write = 0;
        struct pipe *pp = pipe_from_fd(fde, &is_write);
        if (!pp || is_write) return -EBADF;
        long r = pipe_read(pp, buf, count);
        if (r != -EAGAIN) return r;
        if (fde->flags & O_NONBLOCK) return -EAGAIN;
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
            thread_t *t = thread_current();
            pty_t *pty = pty_get(pty_id);
            if (!t || !pty) return -EAGAIN;

            mutex_lock(&pty->lock);
            int avail = (pty->input_tail - pty->input_head
                         + PTY_BUF_SIZE) % PTY_BUF_SIZE;
            if (avail > 0) {
                int n = avail > (int)want ? (int)want : avail;
                for (int i = 0; i < n; i++) {
                    kbuf[i] = (uint8_t)pty->input_buf[pty->input_head];
                    pty->input_head = (pty->input_head + 1) % PTY_BUF_SIZE;
                }
                mutex_unlock(&pty->lock);
                copy_to_user(buf, kbuf, (size_t)n);
                extern void vt_flush(int vt_id);
                vt_flush(pty_id);
                return (long)n;
            }
            pty->blocked_reader = t;
            mutex_unlock(&pty->lock);

            extern void vt_flush(int vt_id);
            vt_flush(pty_id);

            if (t->proc) {
                uint64_t deliverable = t->proc->sig_pending & ~t->sig_blocked;
                if (deliverable) return -EINTR;
            }

            event_t ev;
            int _wr = event_wait(&t->eq, &ev, -1);
            if (_wr == -4) return -EINTR;
        }
    }
    return -EBADF;
}

long do_readv(int fd, const struct iovec *iov, int iovcnt) {
    if (iovcnt < 0 || iovcnt > 1024) return -EINVAL;
    if (iovcnt > 64) return -EINVAL;
    struct iovec kiov[64];
    { int r = copy_from_user(kiov, iov, (size_t)iovcnt * sizeof(struct iovec)); if (r) return r; }
    long total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!user_ok((uint64_t)kiov[i].iov_base, kiov[i].iov_len)) return -EFAULT;
        long r = do_read(fd, (void *)kiov[i].iov_base, kiov[i].iov_len);
        if (r < 0) return total > 0 ? total : r;
        total += r;
        if ((size_t)r < kiov[i].iov_len) break;
    }
    return total;
}

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

long do_open(const char *path, int flags, int mode) {
    char kpath[PATH_MAX], rpath[PATH_MAX];
    int len = copy_path_from_user(kpath, path, PATH_MAX);
    if (len < 0) return len;
    resolve_path(kpath, rpath, PATH_MAX);
    return vfs_open(rpath, flags, mode);
}

long do_openat(int dirfd, const char *path, int flags, int mode) {
    char kpath[PATH_MAX], rpath[PATH_MAX];
    int len = resolve_at_path(dirfd, path, kpath, PATH_MAX);
    if (len < 0) return len;
    resolve_path(kpath, rpath, PATH_MAX);
    return vfs_open(rpath, flags, mode);
}

long do_lseek(int fd, long offset, int whence) {
    return vfs_lseek(fd, offset, whence);
}

long do_dup3(int oldfd, int newfd, int flags) {
    if (oldfd == newfd) return -EINVAL;
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    if (oldfd < 0 || oldfd >= FD_MAX || newfd < 0 || newfd >= FD_MAX) return -EBADF;
    fd_entry_t *old = fd_get(&p->fds, oldfd);
    if (!old) return -EBADF;

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

    p->fds.entries[newfd] = *old;
    p->fds.entries[newfd].flags &= ~O_CLOEXEC;
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

long do_getcwd(char *buf, size_t size) {
    if (!buf) return -EFAULT;
    if (size == 0) return -ERANGE;
    if (!user_ok((uint64_t)buf, size)) return -EFAULT;
    int r = vfs_getcwd(buf, size);
    if (r < 0) return r;
    return (long)(r + 1);
}

long do_chdir(const char *path) {
    char kpath[PATH_MAX], rpath[PATH_MAX];
    int len = copy_path_from_user(kpath, path, PATH_MAX);
    if (len < 0) return len;
    resolve_path(kpath, rpath, PATH_MAX);
    return vfs_chdir(rpath);
}

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[1];
};

static size_t emit_dirent(uint8_t *out, size_t remaining,
                          uint64_t ino, uint64_t off, uint8_t d_type,
                          const char *name) {
    int nlen = 0;
    while (name[nlen]) nlen++;
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
    for (size_t i = 19 + (size_t)nlen + 1; i < reclen; i++)
        ((uint8_t *)ent)[i] = 0;
    return reclen;
}

struct getdents_ctx {
    uint8_t *out;
    size_t   count;
    size_t   written;
    uint64_t next_off;
    int      full;
};

static int getdents_cb(const char *name, uint32_t ino, uint8_t file_type,
                       uint32_t next_pos, void *arg) {
    struct getdents_ctx *ctx = (struct getdents_ctx *)arg;
    uint8_t d_type = 0;
    if (file_type == EXT2_FT_REG_FILE) d_type = 8;
    else if (file_type == EXT2_FT_DIR) d_type = 4;
    else if (file_type == EXT2_FT_SYMLINK) d_type = 10;
    else d_type = 8;

    size_t n = emit_dirent(ctx->out + ctx->written, ctx->count - ctx->written,
                           ino, (uint64_t)next_pos, d_type, name);
    if (n == 0) { ctx->full = 1; return 1; }
    ctx->next_off = (uint64_t)next_pos;
    ctx->written += n;
    return 0;
}

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
                           new_off, new_off, 8 , name);
    if (n == 0) return 1;
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

    if (fde->type == FD_PROCFS) {
        procfs_fd_t *pf = (procfs_fd_t *)fde->obj;
        if (!pf) return -EBADF;

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
                                       (uint64_t)(i + 1), ctx.next_off, 10 , name);
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

    if (f->backend == VFS_BACKEND_EXT2) {
        if (f->type != VFS_DIR) return -ENOTDIR;
        struct getdents_ctx ctx = {
            .out = (uint8_t *)buf,
            .count = count,
            .written = 0,
            .next_off = f->offset,
            .full = 0
        };
        ext2_dir_iterate((uint32_t)f->disk_ino, (uint32_t)f->offset, getdents_cb, &ctx);
        f->offset = ctx.next_off;
        return (long)ctx.written;
    }

    if (!f->node || f->node->type != VFS_DIR) return -ENOTDIR;

    struct vfs_node *dir = f->node;
    uint8_t *out = (uint8_t *)buf;
    size_t written = 0;

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
        if (fde->type == FD_SERIAL) {
            if (!user_ok(arg, 36)) return -EFAULT;
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
    if (request == TCSETS || request == TCSETSW || request == TCSETSF) {
        if (fde->type == FD_PTY_SLAVE || fde->type == FD_PTY_MASTER) {
            struct kernel_termios kterm;
            if (copy_from_user(&kterm, (const void *)arg, 36) == 0) {
                pty_t *pt = pty_get((int)(long)fde->obj);
                if (pt) {
                    mutex_lock(&pt->lock);
                    int was_canon = (pt->termios.c_lflag & ICANON) != 0;
                    int new_canon = (kterm.c_lflag & ICANON) != 0;
                    kmemcpy(&pt->termios, &kterm, sizeof(struct kernel_termios));
                    if (was_canon && !new_canon && pt->line_pos > 0) {
                        for (int li = 0; li < pt->line_pos; li++) {
                            if (((pt->input_tail + 1) % PTY_BUF_SIZE) != pt->input_head)
                                pt->input_buf[pt->input_tail] = pt->line_buf[li],
                                pt->input_tail = (pt->input_tail + 1) % PTY_BUF_SIZE;
                        }
                        pt->line_pos = 0;
                    }
                    mutex_unlock(&pt->lock);
                }
            }
        }
        return 0;
    }
    if (request == TIOCSCTTY || request == TIOCNOTTY)
        return 0;
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
    if (request == TIOCGSID) {
        int32_t sid = (int32_t)p->sid;
        return copy_to_user((void *)arg, &sid, 4);
    }
    if (request == TIOCOUTQ) {
        if (!user_ok(arg, 4)) return -EFAULT;
        *(int *)arg = 0;
        return 0;
    }
    if (request == 0x5421) {
        if (!user_ok(arg, 4)) return -EFAULT;
        int on = *(int *)arg;
        if (on) fde->flags |= O_NONBLOCK;
        else    fde->flags &= ~O_NONBLOCK;
        return 0;
    }
    return -ENOTTY;
}

#define FLOCK_MAX 64

struct flock_entry {
    uint64_t ino;
    uint32_t pid;
    short    type;
    long     start;
    long     len;
};

static struct flock_entry flock_table[FLOCK_MAX];

static uint64_t flock_ino(fd_entry_t *fde) {
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return 0;
    if (f->disk_ino) return f->disk_ino;
    return (uint64_t)(uintptr_t)f->node;
}

static int flock_overlap(long s1, long l1, long s2, long l2) {
    long e1 = l1 ? s1 + l1 : __LONG_MAX__;
    long e2 = l2 ? s2 + l2 : __LONG_MAX__;
    return s1 < e2 && s2 < e1;
}

static struct flock_entry *flock_conflict(uint64_t ino, uint32_t pid,
                                           short type, long start, long len) {
    for (int i = 0; i < FLOCK_MAX; i++) {
        struct flock_entry *e = &flock_table[i];
        if (!e->ino || e->ino != ino || e->pid == pid) continue;
        if (!flock_overlap(e->start, e->len, start, len)) continue;
        if (e->type == F_RDLCK && type == F_RDLCK) continue;
        return e;
    }
    return (void *)0;
}

static long flock_setlk(uint64_t ino, uint32_t pid,
                         short type, long start, long len) {
    if (type == F_UNLCK) {
        for (int i = 0; i < FLOCK_MAX; i++) {
            struct flock_entry *e = &flock_table[i];
            if (e->ino == ino && e->pid == pid &&
                flock_overlap(e->start, e->len, start, len))
                e->ino = 0;
        }
        return 0;
    }
    if (flock_conflict(ino, pid, type, start, len))
        return -EAGAIN;
    int free_slot = -1;
    for (int i = 0; i < FLOCK_MAX; i++) {
        struct flock_entry *e = &flock_table[i];
        if (e->ino == ino && e->pid == pid &&
            flock_overlap(e->start, e->len, start, len)) {
            e->type = type;
            e->start = start;
            e->len = len;
            return 0;
        }
        if (!e->ino && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return -ENOLCK;
    flock_table[free_slot] = (struct flock_entry){
        .ino = ino, .pid = pid, .type = type, .start = start, .len = len
    };
    return 0;
}

void flock_release(uint64_t ino, uint32_t pid) {
    for (int i = 0; i < FLOCK_MAX; i++) {
        struct flock_entry *e = &flock_table[i];
        if (e->ino == ino && e->pid == pid)
            e->ino = 0;
    }
}

void flock_release_pid(uint32_t pid) {
    for (int i = 0; i < FLOCK_MAX; i++)
        if (flock_table[i].pid == pid)
            flock_table[i].ino = 0;
}

long do_fcntl(int fd, int cmd, long arg) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;

    switch (cmd) {
    case F_GETFL: return fde->flags & ~O_CLOEXEC;
    case F_SETFL: {
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
        uint64_t ino = (fde->type == FD_FILE) ? flock_ino(fde) : 0;
        if (!ino) { fl->l_type = F_UNLCK; return 0; }
        struct flock_entry *c = flock_conflict(ino, p->pid,
                                                fl->l_type, fl->l_start, fl->l_len);
        if (c) {
            fl->l_type   = c->type;
            fl->l_whence = 0;
            fl->l_start  = c->start;
            fl->l_len    = c->len;
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
        uint64_t ino = (fde->type == FD_FILE) ? flock_ino(fde) : 0;
        if (!ino) return 0;
        return flock_setlk(ino, p->pid, fl->l_type, fl->l_start, fl->l_len);
    }
    case F_GETOWN:
        return 0;
    case F_SETOWN:
        return 0;
    case F_GETPIPE_SZ:
        return 65536;
    case F_SETPIPE_SZ:
        return 65536;
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        int i = fd_find_free(&p->fds, (int)arg);
        if (i < 0) return -EMFILE;
        p->fds.entries[i] = *fde;
        p->fds.entries[i].flags &= ~O_CLOEXEC;
        fd_mark_used(&p->fds, i);
        if (cmd == F_DUPFD_CLOEXEC)
            p->fds.entries[i].flags |= O_CLOEXEC;
        if (i >= p->fds.max_fd) p->fds.max_fd = i + 1;
        if (fde->type == FD_FILE && fde->obj) {
            extern void vfs_file_incref(struct vfs_file *f);
            vfs_file_incref((struct vfs_file *)fde->obj);
        }
        return i;
    }
    default: return -EINVAL;
    }
}

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

long do_fchdir(int fd) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;
    if (fde->type != FD_FILE) return -ENOTDIR;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f || f->type != VFS_DIR) return -ENOTDIR;
    if (!f->path[0]) return -EBADF;
    int i = 0;
    while (f->path[i] && i < 255) { p->cwd[i] = f->path[i]; i++; }
    p->cwd[i] = '\0';
    return 0;
}

long do_creat(const char *path, int mode) {
    return do_open(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

long do_getdents(int fd, void *buf, size_t count) {
    return do_getdents64(fd, buf, count);
}

long do_close_range(unsigned int first, unsigned int last, unsigned int flags) {
    (void)flags;
    if (first > last) return -EINVAL;
    if (last >= FD_MAX) last = FD_MAX - 1;
    for (unsigned int fd = first; fd <= last; fd++)
        do_close((int)fd);
    return 0;
}

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

long do_memfd_create(const char *uname, unsigned int flags) {
    (void)uname; (void)flags;
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    static int memfd_counter;
    int id = __sync_fetch_and_add(&memfd_counter, 1);

    char path[64];
    char *w = path;
    const char *prefix = "/dev/shm/memfd:";
    while (*prefix) *w++ = *prefix++;
    { int v = id; char t[12]; int ti = 0;
      do { t[ti++] = '0' + (char)(v % 10); v /= 10; } while (v);
      while (ti--) *w++ = t[ti]; }
    *w = '\0';

    struct vfs_node *node = vfs_create(path, VFS_FILE);
    if (!node) return -ENOMEM;

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
