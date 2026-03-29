/* CosmoRT — Stub syscalls (no-op or fixed return values) */

#include "internal.h"

long do_set_robust_list(void *head, size_t len) {
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    if (len != 24) return -EINVAL; /* sizeof(struct robust_list_head) on x86_64 */
    if (head && !user_ok((uint64_t)head, len)) return -EFAULT;
    t->robust_list = head;
    return 0;
}

long do_get_robust_list(int pid, void **head_ptr, size_t *len_ptr) {
    if (pid != 0) return -ESRCH; /* only current thread for now */
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    if (head_ptr) copy_to_user(head_ptr, &t->robust_list, sizeof(void *));
    if (len_ptr) { size_t sz = 24; copy_to_user(len_ptr, &sz, sizeof(sz)); }
    return 0;
}
long do_mount(void)           { return 0; }
long do_sethostname(void)     { return 0; }
long do_rseq(void)            { return -ENOSYS; }
long do_capget(void)          { return -EPERM; }
long do_capset(void)          { return -EPERM; }

/* advisory locks: noop, single-user */
long do_flock(int fd, int operation) {
    (void)fd; (void)operation;
    return 0;
}

/* msync: moved to sys_mem.c (SH-C3: dirty tracking + write-back) */
/* TODO: implement if needed */
long do_sendfile(void) { return -ENOSYS; }
/* single-user: noop */
long do_lchown(void)   { return 0; }

long do_sched_get_priority_max(int policy) { (void)policy; return 31; }
long do_sched_get_priority_min(int policy) { (void)policy; return 0; }

/* noop: no enforcement */
long do_setrlimit(void) { return 0; }
/* no-op: no page cache hints */
long do_fadvise64(void) { return 0; }

/* single-user: return default, ignore new */
long do_umask(int mask) { (void)mask; return 0022; }

long do_getgroups(void) { return 0; }
long do_setgroups(void) { return 0; }

/* personality(2): PER_LINUX = 0 */
long do_personality(unsigned long persona) {
    if (persona == 0xFFFFFFFF) return 0; /* query: return PER_LINUX */
    if (persona == 0) return 0;          /* set PER_LINUX: ok */
    return -EINVAL;
}

/* priority: single-user, no priority enforcement */
long do_getpriority(int which, int who) { (void)which; (void)who; return 20; }
long do_setpriority(int which, int who, int prio) {
    (void)which; (void)who; (void)prio; return 0;
}

/* sync/syncfs/fsync/fdatasync: flush to disk (noop for ramfs, ext2_sync for ext2) */
long do_sync(void) {
    extern void ext2_sync(void);
    ext2_sync();
    return 0;
}

long do_syncfs(int fd) { (void)fd; return do_sync(); }

long do_fsync(int fd) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;
    /* ext2 backend: sync. ramfs/other: noop */
    if (fde->type == FD_FILE) {
        struct vfs_file *f = (struct vfs_file *)fde->obj;
        if (f && f->backend == VFS_BACKEND_EXT2) {
            extern void ext2_sync(void);
            ext2_sync();
        }
    }
    return 0;
}

long do_fdatasync(int fd) { return do_fsync(fd); }

/* umount2: single mount, never unmount — return 0 */
long do_umount2(const char *target, int flags) {
    (void)target; (void)flags; return 0;
}

/* ── Batch 2: remaining FEHLT syscalls ── */

/* mincore: pretend all pages are resident */
long do_mincore(void) { return 0; }

/* ptrace: no debugging support */
long do_ptrace(void) { return -EPERM; }

/* syslog: no kernel log buffer */
long do_syslog_stub(void) { return 0; }

/* sched_rr_get_interval: write 10ms quantum */
long do_sched_rr_get_interval(int pid, void *tp) {
    (void)pid;
    if (!tp) return -EFAULT;
    int64_t ts[2] = { 0, 10000000 }; /* 10ms */
    copy_to_user(tp, ts, sizeof(ts));
    return 0;
}

/* vhangup: no-op */
long do_vhangup(void) { return 0; }

/* adjtimex: no-op */
long do_adjtimex(void) { return 0; }

/* chroot: single-user, no real chroot */
long do_chroot(void) { return 0; }

/* acct: no-op */
long do_acct(void) { return 0; }

/* settimeofday: no-op */
long do_settimeofday(void) { return 0; }

/* setdomainname: no-op */
long do_setdomainname(void) { return 0; }

/* readahead: no-op */
long do_readahead(void) { return 0; }

/* restart_syscall: return -EINTR */
long do_restart_syscall(void) { return -EINTR; }

/* clock_settime: no-op */
long do_clock_settime(void) { return 0; }

/* clock_adjtime: no-op */
long do_clock_adjtime(void) { return 0; }

/* unshare: no-op (single process namespace) */
long do_unshare_stub(void) { return 0; }

/* utimes: delegate to utimensat */
long do_utimes(const char *filename, const void *utimes_buf) {
    if (utimes_buf) {
        /* struct timeval { time_t tv_sec; suseconds_t tv_usec; } x2 */
        long tv[4];
        int r = copy_from_user(tv, utimes_buf, sizeof(tv));
        if (r) return r;
        int64_t ts[4] = { tv[0], tv[1] * 1000, tv[2], tv[3] * 1000 };
        return do_utimensat(AT_FDCWD, filename, ts, 0);
    }
    return do_utimensat(AT_FDCWD, filename, 0, 0);
}

/* futimesat: delegate to utimensat */
long do_futimesat(int dirfd, const char *filename, const void *utimes_buf) {
    if (utimes_buf) {
        long tv[4];
        int r = copy_from_user(tv, utimes_buf, sizeof(tv));
        if (r) return r;
        int64_t ts[4] = { tv[0], tv[1] * 1000, tv[2], tv[3] * 1000 };
        return do_utimensat(dirfd, filename, ts, 0);
    }
    return do_utimensat(dirfd, filename, 0, 0);
}

/* signalfd: delegate to signalfd4 */
long do_signalfd(int fd, const uint64_t *mask) {
    extern long do_signalfd4(int fd, const uint64_t *mask, int flags);
    return do_signalfd4(fd, mask, 0);
}

/* eventfd: delegate to eventfd2 */
long do_eventfd(unsigned int initval) {
    extern long do_eventfd2(unsigned int initval, int flags);
    return do_eventfd2(initval, 0);
}

/* timerfd_gettime: return zeroed itimerspec */
long do_timerfd_gettime(int fd, void *curr_value) {
    (void)fd;
    if (!curr_value) return -EFAULT;
    int64_t zero[4] = { 0, 0, 0, 0 };
    copy_to_user(curr_value, zero, sizeof(zero));
    return 0;
}

/* rt_tgsigqueueinfo: delegate to rt_sigqueueinfo */
long do_rt_tgsigqueueinfo(int tgid, int tid, int sig, void *uinfo) {
    (void)tid; /* ignore tid, dispatch to process-level */
    extern long do_rt_sigqueueinfo(int pid, int sig, void *uinfo);
    return do_rt_sigqueueinfo(tgid, sig, uinfo);
}

/* epoll_create: delegate to epoll_create1 */
long do_epoll_create(int size) {
    (void)size;
    extern long do_epoll_create1(int flags);
    return do_epoll_create1(0);
}

/* inotify_init: delegate to inotify_init1 */
long do_inotify_init(void) {
    extern long do_inotify_init1(int flags);
    return do_inotify_init1(0);
}

/* preadv2/pwritev2: delegate to preadv/pwritev (ignore flags) */
long do_preadv2(int fd, const void *iov, int iovcnt) {
    return do_readv(fd, (const struct iovec *)iov, iovcnt);
}
long do_pwritev2(int fd, const void *iov, int iovcnt) {
    return do_writev(fd, (const struct iovec *)iov, iovcnt);
}

/* openat2: delegate to openat (ignore resolve flags) */
long do_openat2(int dirfd, const char *pathname, void *how, size_t size) {
    (void)size;
    if (!how) return -EFAULT;
    /* struct open_how { u64 flags; u64 mode; u64 resolve; } */
    uint64_t fields[3];
    int r = copy_from_user(fields, how, sizeof(fields));
    if (r) return r;
    extern long do_openat(int dirfd, const char *path, int flags, int mode);
    return do_openat(dirfd, pathname, (int)fields[0], (int)fields[1]);
}

/* epoll_pwait2: delegate to epoll_wait (ignore sigmask + timespec precision) */
long do_epoll_pwait2(int epfd, void *events, int maxevents, void *timeout) {
    int timeout_ms = -1;
    if (timeout) {
        int64_t ts[2];
        int r = copy_from_user(ts, timeout, sizeof(ts));
        if (r) return r;
        timeout_ms = (int)(ts[0] * 1000 + ts[1] / 1000000);
    }
    return do_epoll_wait(epfd, (struct epoll_event *)events, maxevents, timeout_ms);
}

/* mknod: delegate to mknodat */
long do_mknod(const char *path, uint32_t mode, uint64_t dev) {
    extern long do_mknodat(int dirfd, const char *path, uint32_t mode, uint64_t dev);
    return do_mknodat(AT_FDCWD, path, mode, dev);
}

/* fchmodat2: delegate to fchmodat */
long do_fchmodat2(int dirfd, const char *path, uint32_t mode, int flags) {
    extern long do_fchmodat(int dirfd, const char *path, uint32_t mode, int flags);
    return do_fchmodat(dirfd, path, mode, flags);
}

/* utime: delegate to utimensat */
long do_utime(const char *filename, const void *times) {
    /* struct utimbuf { time_t actime; time_t modtime; } */
    if (times) {
        long utimes[2];
        int r = copy_from_user(utimes, times, sizeof(utimes));
        if (r) return r;
        int64_t ts[4] = { utimes[0], 0, utimes[1], 0 };
        return do_utimensat(AT_FDCWD, filename, ts, 0);
    }
    return do_utimensat(AT_FDCWD, filename, 0, 0);
}
