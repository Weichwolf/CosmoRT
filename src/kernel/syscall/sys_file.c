/* CosmoRT Syscall Layer — file I/O syscalls */

#include "internal.h"

/* Resolve a relative path against CWD, handling "." and ".." components.
 * Result written to out (max outsize bytes). Returns 0 on success. */
static int resolve_path(const char *path, char *out, int outsize) {
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

/* Device file IDs (must match vfs.c) */
#define DEV_NULL    1
#define DEV_ZERO    2
#define DEV_URANDOM 3
#define DEV_TTY     4

long do_write(int fd, const void *buf, size_t count) {
    if (!user_ok((uint64_t)buf, count)) return -EFAULT;
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;
    if (fde->type == FD_DEVICE) {
        int devid = (int)(uintptr_t)fde->obj;
        if (devid == DEV_NULL) return (long)count; /* swallow */
        if (devid == DEV_TTY)  { /* write to serial */
            size_t actual = count > 0x10000 ? 0x10000 : count;
            uint8_t kbuf[256]; size_t pos = 0;
            while (pos < actual) {
                size_t chunk = actual - pos > 256 ? 256 : actual - pos;
                kmemcpy(kbuf, (const uint8_t *)buf + pos, chunk);
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
            kmemcpy(kbuf, (const uint8_t *)buf + pos, chunk);
            for (size_t j = 0; j < chunk; j++) serial_putchar((char)kbuf[j]);
            pos += chunk;
        }
        return (long)actual;
    }
    if (fde->type == FD_FILE)
        return vfs_write(fd, buf, count);
    if (fde->type == FD_SOCKET)
        return socket_write(fd, buf, (long)count);
    if (fde->type == FD_UNIX_SOCK)
        return usock_write(fd, buf, (long)count);
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
            kmemcpy(kbuf, (const uint8_t *)buf + pos, chunk);
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
    if (!user_ok((uint64_t)iov, (size_t)iovcnt * sizeof(struct iovec))) return -EFAULT;
    /* Copy iov array to kernel stack to prevent TOCTOU */
    struct iovec k_iov[16];
    kmemcpy(k_iov, iov, (size_t)iovcnt * sizeof(struct iovec));
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

long do_read(int fd, void *buf, size_t count) {
    if (!user_ok((uint64_t)buf, count)) return -EFAULT;
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;
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
        kmemcpy(buf, kbuf, got);
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
        int got = procfs_read(pf->handle, kbuf, want, pf->offset);
        if (got > 0) {
            kmemcpy(buf, kbuf, (size_t)got);
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
        /* Blocking: restart syscall when peer writes */
        return -EAGAIN; /* TODO: proper blocking */
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
        int got = pty_slave_read(pty_id, (char *)kbuf, (int)want);
        if (got > 0) {
            kmemcpy(buf, kbuf, (size_t)got);
            /* Flush VT after read — renders echo from line discipline */
            extern void vt_flush(int vt_id);
            vt_flush(pty_id);
            return (long)got;
        }
        /* No data — block until pty_master_write wakes us.
         * Thread resumes at userspace with rax=-EAGAIN; the read loop
         * in vt_shell (or libc) retries the syscall. */
        {
            extern uint64_t pml4[];
            thread_t *t = thread_current();
            pty_t *pty = pty_get(pty_id);
            if (t && pty) {
                uint64_t irqf;
                spin_lock_irq(&pty->lock, &irqf);
                /* Re-check under lock — data may have arrived between
                 * the unlocked pty_slave_read and acquiring the lock */
                int avail = (pty->input_tail - pty->input_head
                             + PTY_BUF_SIZE) % PTY_BUF_SIZE;
                if (avail > 0) {
                    int n = avail > (int)want ? (int)want : avail;
                    for (int i = 0; i < n; i++) {
                        kbuf[i] = (uint8_t)pty->input_buf[pty->input_head];
                        pty->input_head = (pty->input_head + 1) % PTY_BUF_SIZE;
                    }
                    spin_unlock_irq(&pty->lock, irqf);
                    kmemcpy(buf, kbuf, (size_t)n);
                    extern void vt_flush(int vt_id);
                    vt_flush(pty_id);
                    return (long)n;
                }
                pty->blocked_reader = t;
                spin_unlock_irq(&pty->lock, irqf);

                /* Flush VT before blocking — renders echo from
                 * keyboard input that woke us without line data */
                extern void vt_flush(int vt_id);
                vt_flush(pty_id);

                save_user_state_for_block(t, -EAGAIN);
                t->state = THREAD_BLOCKED;
                __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(pml4)) : "memory");
                thread_return_to_kernel(t);
            }
        }
        return -EAGAIN;
    }
    return -EBADF;
}

/* ── SYS_readv (19) ──────────────────────────────── */

long do_readv(int fd, const struct iovec *iov, int iovcnt) {
    if (iovcnt < 0 || iovcnt > 1024) return -EINVAL;
    if (!user_ok((uint64_t)iov, (size_t)iovcnt * sizeof(struct iovec))) return -EFAULT;
    /* Copy iov to kernel to prevent TOCTOU — cap at 64 on stack */
    if (iovcnt > 64) iovcnt = 64;
    struct iovec kiov[64];
    kmemcpy(kiov, iov, (size_t)iovcnt * sizeof(struct iovec));
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
    int len = copy_path_from_user(kpath, path, PATH_MAX);
    if (len < 0) return len;
    /* Absolute paths ignore dirfd; relative paths require AT_FDCWD.
     * TODO: real dirfd support (resolve path relative to open directory fd) */
    if (kpath[0] != '/' && dirfd != AT_FDCWD) return -EBADF;
    return vfs_open(kpath, flags, mode);
}

/* ── SYS_lseek (8) ──────────────────────────────── */

long do_lseek(int fd, long offset, int whence) {
    return vfs_lseek(fd, offset, whence);
}

/* ── SYS_fstat (5) / SYS_stat (4) ───────────────── */

long do_fstat(int fd, struct k_stat *buf) {
    if (!user_ok((uint64_t)buf, sizeof(struct k_stat))) return -EFAULT;
    return vfs_fstat(fd, buf);
}

/* ── SYS_dup3 (292) — primary; dup2 delegates here via dispatch ── */

long do_dup3(int oldfd, int newfd, int flags) {
    if (oldfd == newfd) return -EINVAL;
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    if (oldfd < 0 || oldfd >= FD_MAX || newfd < 0 || newfd >= FD_MAX) return -EBADF;
    fd_entry_t *old = fd_get(&p->fds, oldfd);
    if (!old) return -EBADF;

    /* Close newfd if open */
    fd_entry_t *cur = fd_get(&p->fds, newfd);
    if (cur) {
        if (cur->type == FD_FILE) vfs_close(newfd);
        else if (cur->type == FD_PIPE) pipe_close(cur);
        else fd_close(&p->fds, newfd);
        p->fds.entries[newfd].type = FD_NONE;
        p->fds.entries[newfd].obj = 0;
    }

    /* Copy the fd entry and bump refcount */
    p->fds.entries[newfd] = *old;
    if (old->type == FD_FILE && old->obj) {
        extern void vfs_file_incref(struct vfs_file *f);
        vfs_file_incref((struct vfs_file *)old->obj);
    } else if (old->type == FD_PIPE && old->obj) {
        fd_obj_incref(FD_PIPE, old->obj);
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
    return (long)(uint64_t)buf; /* Linux returns pointer */
}

long do_chdir(const char *path) {
    char kpath[PATH_MAX];
    int len = copy_path_from_user(kpath, path, PATH_MAX);
    if (len < 0) return len;
    return vfs_chdir(kpath);
}

/* ── SYS_fstatat (262) — primary; stat/lstat delegate here via dispatch ── */

long do_fstatat(int dirfd, const char *path, struct k_stat *buf, int flags) {
    char kpath[PATH_MAX], rpath[PATH_MAX];
    int len = copy_path_from_user(kpath, path, PATH_MAX);
    if (len < 0) return len;
    if (!user_ok((uint64_t)buf, sizeof(struct k_stat))) return -EFAULT;
    /* TODO: real dirfd support */
    if (dirfd != AT_FDCWD && kpath[0] != '/') return -EBADF;
    resolve_path(kpath, rpath, PATH_MAX);
    if (flags & AT_SYMLINK_NOFOLLOW)
        return vfs_lstat(rpath, buf);
    return vfs_stat(rpath, buf);
}

/* ── SYS_mkdir/rmdir/unlink/rename ───────────────── */

/* ── SYS_mkdirat (258) — primary; mkdir delegates here via dispatch ── */

long do_mkdirat(int dirfd, const char *path, int mode) {
    (void)mode;
    char kpath[PATH_MAX];
    int len = copy_path_from_user(kpath, path, PATH_MAX);
    if (len < 0) return len;
    /* TODO: real dirfd support */
    if (dirfd != AT_FDCWD && kpath[0] != '/') return -EBADF;
    return vfs_mkdir(kpath);
}

/* ── SYS_unlinkat (263) — primary; unlink/rmdir delegate here via dispatch ── */

long do_unlinkat(int dirfd, const char *path, int flags) {
    char kpath[PATH_MAX];
    int len = copy_path_from_user(kpath, path, PATH_MAX);
    if (len < 0) return len;
    /* TODO: real dirfd support */
    if (dirfd != AT_FDCWD && kpath[0] != '/') return -EBADF;
    if (flags & AT_REMOVEDIR)
        return vfs_rmdir(kpath);
    return vfs_unlink(kpath);
}

/* ── SYS_renameat2 (316) — primary; rename delegates here via dispatch ── */

long do_renameat2(int olddirfd, const char *oldpath,
                          int newdirfd, const char *newpath, int flags) {
    (void)flags; /* TODO: RENAME_NOREPLACE etc. */
    char kold[PATH_MAX], knew[PATH_MAX];
    int r = copy_path_from_user(kold, oldpath, PATH_MAX);
    if (r < 0) return r;
    r = copy_path_from_user(knew, newpath, PATH_MAX);
    if (r < 0) return r;
    /* TODO: real dirfd support */
    if (olddirfd != AT_FDCWD && kold[0] != '/') return -EBADF;
    if (newdirfd != AT_FDCWD && knew[0] != '/') return -EBADF;
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

/* ── SYS_link (86) ───────────────────────────────── */

/* ── SYS_linkat (265) — primary; link delegates here via dispatch ── */

long do_linkat(int olddirfd, const char *oldpath,
               int newdirfd, const char *newpath, int flags) {
    (void)flags; /* TODO: AT_EMPTY_PATH, AT_SYMLINK_FOLLOW */
    char kold[PATH_MAX], knew[PATH_MAX];
    int r = copy_path_from_user(kold, oldpath, PATH_MAX);
    if (r < 0) return r;
    r = copy_path_from_user(knew, newpath, PATH_MAX);
    if (r < 0) return r;
    /* TODO: real dirfd support */
    if (olddirfd != AT_FDCWD && kold[0] != '/') return -EBADF;
    if (newdirfd != AT_FDCWD && knew[0] != '/') return -EBADF;
    return vfs_link(kold, knew);
}

/* ── SYS_symlink (88) ───────────────────────────── */

/* ── SYS_symlinkat (266) — primary; symlink delegates here via dispatch ── */

long do_symlinkat(const char *target, int newdirfd, const char *linkpath) {
    char ktarget[PATH_MAX], klink[PATH_MAX];
    int r = copy_path_from_user(ktarget, target, PATH_MAX);
    if (r < 0) return r;
    r = copy_path_from_user(klink, linkpath, PATH_MAX);
    if (r < 0) return r;
    /* TODO: real dirfd support */
    if (newdirfd != AT_FDCWD && klink[0] != '/') return -EBADF;
    return vfs_symlink(ktarget, klink);
}

/* ── SYS_readlink (89) ──────────────────────────── */

/* ── SYS_readlinkat (267) — primary; readlink delegates here via dispatch ── */

long do_readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz) {
    char kpath[PATH_MAX];
    int r = copy_path_from_user(kpath, path, PATH_MAX);
    if (r < 0) return r;
    if (!user_ok((uint64_t)buf, bufsiz)) return -EFAULT;
    /* TODO: real dirfd support */
    if (dirfd != AT_FDCWD && kpath[0] != '/') return -EBADF;
    return vfs_readlink(kpath, buf, bufsiz);
}

/* ── SYS_truncate (76) / SYS_ftruncate (77) ─────── */

long do_truncate(const char *path, int64_t length) {
    char kpath[PATH_MAX];
    int r = copy_path_from_user(kpath, path, PATH_MAX);
    if (r < 0) return r;
    return vfs_truncate(kpath, length);
}

long do_ftruncate(int fd, int64_t length) {
    return vfs_ftruncate(fd, length);
}

/* ── SYS_fchmodat (268) ─────────────────────────── */

long do_fchmodat(int dirfd, const char *path, uint32_t mode, int flags) {
    (void)flags;
    char kpath[PATH_MAX];
    int r = copy_path_from_user(kpath, path, PATH_MAX);
    if (r < 0) return r;
    /* TODO: real dirfd support */
    if (dirfd != AT_FDCWD && kpath[0] != '/') return -EBADF;
    return vfs_chmod(kpath, mode);
}

/* ── SYS_utimensat (280) ────────────────────────── */

long do_utimensat(int dirfd, const char *path, const void *utimes, int flags) {
    if (!path) return 0; /* futimens with NULL path = no-op for now */

    char kpath[PATH_MAX];
    int r = copy_path_from_user(kpath, path, PATH_MAX);
    if (r < 0) return r;
    /* TODO: real dirfd support */
    if (dirfd != AT_FDCWD && kpath[0] != '/') return -EBADF;

    int64_t ktimes[4];
    if (utimes) {
        if (!user_ok((uint64_t)utimes, 32)) return -EFAULT;
        kmemcpy(ktimes, utimes, 32); /* 2 × struct timespec = 2 × 16 bytes */
    }

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
    int r = copy_path_from_user(kpath, path, PATH_MAX);
    if (r < 0) return r;
    /* TODO: real dirfd support */
    if (dirfd != AT_FDCWD && kpath[0] != '/') return -EBADF;

    /* Create as regular file via open+close */
    int fd = vfs_open(kpath, O_CREAT | O_WRONLY, (int)mode);
    if (fd < 0) return fd;
    return vfs_close(fd);
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

/* Callback context for CosmoFS getdents64 via cosmofs_dir_iterate */
struct getdents_ctx {
    uint8_t *out;
    size_t   count;
    size_t   written;
    uint64_t next_off;
    int      full;       /* set when buffer is exhausted */
};

static int getdents_cb(const char *name, uint64_t ino, void *arg) {
    struct getdents_ctx *ctx = (struct getdents_ctx *)arg;
    /* Determine type: read inode to check */
    uint8_t d_type = 8; /* DT_REG */
    struct cosmofs_inode *ip = cosmofs_inode_read(ino);
    if (ip && ip->type == COSMOFS_TYPE_DIR) d_type = 4; /* DT_DIR */

    ctx->next_off++;
    size_t n = emit_dirent(ctx->out + ctx->written, ctx->count - ctx->written,
                           ino, ctx->next_off, d_type, name);
    if (n == 0) { ctx->full = 1; return 1; /* stop */ }
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
    ctx->next_off++;
    size_t n = emit_dirent(ctx->out + ctx->written, ctx->count - ctx->written,
                           ctx->next_off, ctx->next_off, 8 /* DT_REG */, name);
    if (n == 0) return 1; /* stop: buffer full */
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
        if (!pf || pf->handle != -1) return -ENOTDIR;
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

    /* CosmoFS directory */
    if (f->backend == VFS_BACKEND_COSMOFS) {
        if (f->type != VFS_DIR) return -ENOTDIR;
        struct getdents_ctx ctx = {
            .out = (uint8_t *)buf,
            .count = count,
            .written = 0,
            .next_off = f->offset,
            .full = 0
        };
        cosmofs_dir_iterate(f->cosmofs_ino, (int)f->offset, getdents_cb, &ctx);
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

/* ── SYS_faccessat (269) — primary; access delegates here via dispatch ── */

long do_faccessat(int dirfd, const char *path, int mode, int flags) {
    (void)mode; (void)flags; /* TODO: real mode/flags checking */
    char kpath[PATH_MAX];
    int len = copy_path_from_user(kpath, path, PATH_MAX);
    if (len < 0) return len;
    /* TODO: real dirfd support */
    if (dirfd != AT_FDCWD && kpath[0] != '/') return -EBADF;
    /* Check ramfs first */
    if (vfs_lookup(kpath)) return 0;
    /* Check procfs entries (/proc/NAME) */
    const char *pname = 0;
    if (kpath[0]=='/' && kpath[1]=='p' && kpath[2]=='r' && kpath[3]=='o' &&
        kpath[4]=='c' && kpath[5]=='/')
        pname = kpath + 6;
    if (pname && procfs_open(pname)) return 0;
    /* Check device special files and virtual dirs */
    {
        static const char *devpaths[] = {
            "/dev/null", "/dev/zero", "/dev/urandom", "/dev/tty",
            "/dev", "/proc", 0
        };
        for (const char **dp = devpaths; *dp; dp++) {
            const char *a = kpath, *b = *dp;
            while (*a && *a == *b) { a++; b++; }
            if (*a == 0 && *b == 0) return 0;
        }
    }
    /* CosmoFS on disk */
    extern uint64_t cosmofs_walk_path(const char *);
    if (cosmofs_walk_path(kpath)) return 0;
    return -ENOENT;
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
#define TIOCNOTTY  0x5422
#define TIOCGPTN   0x80045430
#define TIOCSPTLCK 0x40045431
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030

struct winsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };

long do_ioctl(int fd, unsigned long request, unsigned long arg) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;

    if (request == TCGETS) {
        /* Minimal termios: tell isatty() this is a terminal */
        if (fde->type == FD_SERIAL || fde->type == FD_PTY_SLAVE ||
            fde->type == FD_PTY_MASTER) {
            if (!user_ok(arg, 60)) return -EFAULT;
            kmemset((void *)arg, 0, 60); /* zero-filled termios */
            return 0;
        }
        return -ENOTTY;
    }
    if (request == TIOCGWINSZ) {
        if (!user_ok(arg, sizeof(struct winsize))) return -EFAULT;
        struct winsize *ws = (struct winsize *)arg;
        ws->ws_row = (uint16_t)vt_rows();
        ws->ws_col = (uint16_t)vt_cols();
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        return 0;
    }
    /* Terminal set: accept and ignore (no real termios backend) */
    if (request == TCSETS || request == TCSETSW || request == TCSETSF)
        return 0;
    /* Controlling terminal / process group stubs */
    if (request == TIOCSCTTY || request == TIOCNOTTY ||
        request == TIOCSPGRP)
        return 0;
    if (request == TIOCGPGRP) {
        if (!user_ok(arg, 4)) return -EFAULT;
        process_t *gp = proc_current();
        int32_t pgid = gp ? (int32_t)gp->pgid : 1;
        kmemcpy((void *)arg, &pgid, 4);
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
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        /* Find lowest fd >= arg */
        for (int i = (int)arg; i < FD_MAX; i++) {
            if (!fd_get(&p->fds, i) || p->fds.entries[i].type == FD_NONE) {
                p->fds.entries[i] = *fde;
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
        }
        return -EMFILE;
    }
    default: return -EINVAL;
    }
}
