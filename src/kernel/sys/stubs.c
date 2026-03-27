/* CosmoRT — Stub syscalls (no-op or fixed return values) */

#include "internal.h"

long do_set_robust_list(void) { return 0; }
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
