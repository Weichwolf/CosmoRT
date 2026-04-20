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
#include "linux/capability.h"

/* capget/capset — single-user kernel: all caps always set. Still enforce
 * Linux ABI validation (version, pid, EFAULT). */

static int cap_version_u32s(uint32_t ver) {
    if (ver == _LINUX_CAPABILITY_VERSION_1) return _LINUX_CAPABILITY_U32S_1;
    if (ver == _LINUX_CAPABILITY_VERSION_2) return _LINUX_CAPABILITY_U32S_2;
    if (ver == _LINUX_CAPABILITY_VERSION_3) return _LINUX_CAPABILITY_U32S_3;
    return 0;
}

long do_capget(void *hdrp, void *datap) {
    if (!hdrp) return -EFAULT;
    if (!user_ok((uint64_t)hdrp, sizeof(struct __user_cap_header_struct)))
        return -EFAULT;
    struct __user_cap_header_struct hdr;
    int r = copy_from_user(&hdr, hdrp, sizeof(hdr));
    if (r) return r;

    int u32s = cap_version_u32s(hdr.version);
    if (!u32s) {
        hdr.version = _LINUX_CAPABILITY_VERSION_3;
        copy_to_user(hdrp, &hdr, sizeof(hdr));
        return -EINVAL;
    }
    if (hdr.pid < 0) return -EINVAL;
    if (hdr.pid > 0) {
        process_t *target = proc_find((uint32_t)hdr.pid);
        if (!target) return -ESRCH;
    }
    if (!datap) return 0;
    if (!user_ok((uint64_t)datap, u32s * sizeof(struct __user_cap_data_struct)))
        return -EFAULT;

    struct __user_cap_data_struct data[2] = {
        { .effective = 0xFFFFFFFFu, .permitted = 0xFFFFFFFFu, .inheritable = 0xFFFFFFFFu },
        { .effective = 0xFFFFFFFFu, .permitted = 0xFFFFFFFFu, .inheritable = 0xFFFFFFFFu },
    };
    return copy_to_user(datap, data, u32s * sizeof(struct __user_cap_data_struct));
}

long do_capset(void *hdrp, const void *datap) {
    if (!hdrp) return -EFAULT;
    if (!user_ok((uint64_t)hdrp, sizeof(struct __user_cap_header_struct)))
        return -EFAULT;
    struct __user_cap_header_struct hdr;
    int r = copy_from_user(&hdr, hdrp, sizeof(hdr));
    if (r) return r;

    int u32s = cap_version_u32s(hdr.version);
    if (!u32s) {
        hdr.version = _LINUX_CAPABILITY_VERSION_3;
        copy_to_user(hdrp, &hdr, sizeof(hdr));
        return -EINVAL;
    }
    if (hdr.pid < 0) return -EINVAL;
    /* Linux: capset only supports pid==0 or pid==self — others -EPERM. */
    if (hdr.pid > 0) {
        process_t *p = proc_current();
        if (!p || (uint32_t)hdr.pid != p->pid) return -EPERM;
    }
    if (!datap) return -EFAULT;
    if (!user_ok((uint64_t)datap, u32s * sizeof(struct __user_cap_data_struct)))
        return -EFAULT;

    struct __user_cap_data_struct data[2];
    r = copy_from_user(data, datap, u32s * sizeof(struct __user_cap_data_struct));
    if (r) return r;
    /* Single-user: accept any capset as no-op — all caps already granted. */
    return 0;
}

/* msync: moved to sys_mem.c (SH-C3: dirty tracking + write-back) */
/* TODO: implement if needed */
long do_sendfile(void) { return -ENOSYS; }

long do_sched_get_priority_max(int policy) { (void)policy; return 31; }
long do_sched_get_priority_min(int policy) { (void)policy; return 0; }

/* noop: no enforcement */
long do_setrlimit(void) { return 0; }

long do_umask(int mask) {
    process_t *p = proc_current();
    if (!p) return 0022;
    int old = p->umask_val ? (int)p->umask_val : 0022;
    p->umask_val = mask & 0777;
    return old;
}

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

/* sync/syncfs/fsync/fdatasync: flush to disk (noop for ramfs, ext4_sync for ext4) */
long do_sync(void) {
    extern void ext4_sync(void);
    ext4_sync();
    return 0;
}

long do_syncfs(int fd) { (void)fd; return do_sync(); }

long do_fsync(int fd) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type == FD_NONE) return -EBADF;
    switch (fde->type) {
    case FD_FILE: {
        struct vfs_file *f = (struct vfs_file *)fde->obj;
        if (f && f->backend == VFS_BACKEND_EXT4) {
            extern void ext4_sync(void);
            ext4_sync();
        }
        return 0;
    }
    case FD_PROCFS:
    case FD_DEVICE:
    case FD_SERIAL:
    case FD_PTY_MASTER:
    case FD_PTY_SLAVE:
        return 0;
    default:
        return -EINVAL;
    }
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

/* adjtimex: validate timex pointer, check tick range.
 * struct timex layout mirrors Linux __kernel_timex (see test_adjtimex.c). */
#define ADJ_OFFSET            0x0001
#define ADJ_FREQUENCY         0x0002
#define ADJ_MAXERROR          0x0004
#define ADJ_ESTERROR          0x0008
#define ADJ_STATUS            0x0010
#define ADJ_TIMECONST          0x0020
#define ADJ_TAI                0x0080
#define ADJ_SETOFFSET          0x0100
#define ADJ_MICRO              0x1000
#define ADJ_NANO               0x2000
#define ADJ_TICK               0x4000
#define ADJ_OFFSET_SINGLESHOT  0x8001
#define ADJ_OFFSET_SS_READ     0xa001

#define ADJ_ALL_VALID (ADJ_OFFSET | ADJ_FREQUENCY | ADJ_MAXERROR |        \
                       ADJ_ESTERROR | ADJ_STATUS | ADJ_TIMECONST |         \
                       ADJ_TAI | ADJ_SETOFFSET | ADJ_MICRO | ADJ_NANO |    \
                       ADJ_TICK | ADJ_OFFSET_SS_READ)

#define TIMEX_TICK_DEFAULT     10000L  /* HZ=100 → 1e6/HZ µs */
#define TIMEX_TICK_MIN         9000L
#define TIMEX_TICK_MAX        11000L

struct k_timex {
    unsigned int modes;
    long         offset;
    long         freq;
    long         maxerror;
    long         esterror;
    int          status;
    long         constant;
    long         precision;
    long         tolerance;
    long         time_sec;
    long         time_usec;
    long         tick;
    long         ppsfreq;
    long         jitter;
    int          shift;
    long         stabil;
    long         jitcnt;
    long         calcnt;
    long         errcnt;
    long         stbcnt;
    int          tai;
    int          __padding[11];
};

long do_adjtimex(void *tx) {
    if (!tx) return -EFAULT;
    if (!user_ok((uint64_t)tx, sizeof(struct k_timex))) return -EFAULT;
    struct k_timex ktx;
    int r = copy_from_user(&ktx, tx, sizeof(ktx));
    if (r) return r;
    if (ktx.modes == ADJ_OFFSET_SINGLESHOT) {
        /* legacy compat: accept as TIME_OK */
    } else if (ktx.modes & ~ADJ_ALL_VALID) {
        return -EINVAL;
    }
    if (ktx.modes & ADJ_TICK) {
        if (ktx.tick < TIMEX_TICK_MIN || ktx.tick > TIMEX_TICK_MAX) return -EINVAL;
    }
    ktx.tick = TIMEX_TICK_DEFAULT;
    ktx.tai = 0;
    r = copy_to_user(tx, &ktx, sizeof(ktx));
    if (r) return r;
    return 0; /* TIME_OK */
}

long do_chroot(const char *path) {
    char kpath_raw[PATH_MAX];
    int len = copy_path_from_user(kpath_raw, path, PATH_MAX);
    if (len < 0) return len;
    int comp = 0;
    for (int i = 0; i < len; i++) {
        if (kpath_raw[i] == '/') comp = 0;
        else if (++comp > 255) return -ENAMETOOLONG;
    }
    /* Resolve via existing mechanics (respects current chroot) */
    char kpath[PATH_MAX];
    resolve_path(kpath_raw, kpath, PATH_MAX);

    extern struct vfs_node *vfs_lookup_err(const char *path, int *err);
    int lerr = -ENOENT;
    struct vfs_node *node = vfs_lookup_err(kpath, &lerr);
    if (!node) return lerr;
    if (node->inode->type != VFS_DIR) return -ENOTDIR;

    process_t *p = proc_current();
    if (!p) return -EFAULT;
    int i = 0;
    while (kpath[i] && i < (int)sizeof(p->root) - 1) { p->root[i] = kpath[i]; i++; }
    p->root[i] = '\0';
    /* cwd is now implicitly relative to the new root; reset to "/" */
    p->cwd[0] = '/';
    p->cwd[1] = '\0';
    return 0;
}

long do_acct(const char *path) {
    (void)path;
    return 0;
}

/* settimeofday: no-op */
long do_settimeofday(void) { return 0; }

/* setdomainname: no-op */
long do_setdomainname(void) { return 0; }

/* readahead: no-op */
long do_readahead(void) { return 0; }

/* restart_syscall: return -EINTR */
long do_restart_syscall(void) { return -EINTR; }

/* clock_settime: validate, reject non-settable clocks */
long do_clock_settime(int clk_id, const void *tp) {
    if (!tp) return -EFAULT;
    if (!user_ok((uint64_t)tp, sizeof(struct k_timespec))) return -EFAULT;
    switch (clk_id) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
        break;
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
        return -EINVAL;
    default:
        return -EINVAL;
    }
    struct k_timespec kts;
    int r = copy_from_user(&kts, tp, sizeof(kts));
    if (r) return r;
    if (kts.tv_nsec < 0 || kts.tv_nsec >= NSEC_PER_SEC) return -EINVAL;
    if (kts.tv_sec < 0) return -EINVAL;
    extern uint64_t rtc_epoch_sec;
    uint64_t uptime_sec = timer_ms() / MSEC_PER_SEC;
    rtc_epoch_sec = (uint64_t)kts.tv_sec - uptime_sec;
    return 0;
}

/* clock_adjtime: validate clock_id */
long do_clock_adjtime(int clk_id, void *tx) {
    if (!tx || !user_ok((uint64_t)tx, 4)) return -EFAULT;
    if (clk_id != CLOCK_REALTIME) return -EINVAL;
    return 0;
}

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

/* epoll_create: size must be > 0 (Linux ABI, ignored but validated) */
long do_epoll_create(int size) {
    if (size <= 0) return -EINVAL;
    extern long do_epoll_create1(int flags);
    return do_epoll_create1(0);
}

/* inotify_init: delegate to inotify_init1 */
long do_inotify_init(void) {
    extern long do_inotify_init1(int flags);
    return do_inotify_init1(0);
}

/* preadv2/pwritev2: now handled by do_preadv/do_pwritev in sys_file.c */

/* openat2: delegate to openat (ignore resolve flags) */
long do_openat2(int dirfd, const char *pathname, void *how, size_t size) {
    (void)size;
    if (!how) return -EFAULT;
    /* struct open_how { u64 flags; u64 mode; u64 resolve; } */
    uint64_t fields[3];
    int r = copy_from_user(fields, how, sizeof(fields));
    if (r) return r;
    return do_openat(dirfd, pathname, (int)fields[0], (int)fields[1]);
}

/* epoll_pwait2: delegate to epoll_wait (ignore sigmask + timespec precision) */
long do_epoll_pwait2(int epfd, void *events, int maxevents, void *timeout) {
    int timeout_ms = -1;
    if (timeout) {
        int64_t ts[2];
        int r = copy_from_user(ts, timeout, sizeof(ts));
        if (r) return r;
        if (ts[0] < 0 || ts[1] < 0 || ts[1] >= NSEC_PER_SEC) return -EINVAL;
        timeout_ms = (int)(ts[0] * MSEC_PER_SEC + ts[1] / NSEC_PER_MSEC);
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
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return -EINVAL;
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
