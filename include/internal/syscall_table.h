/* CosmoRT Syscall Table — single source of truth.
 *
 * X(nr, name, nargs, handler)
 *
 * Used by:
 * - dispatch.c: generates switch cases
 * - tracing: logs syscall names via syscall_name()
 *
 * Cases with multi-statement bodies (local variables, HW_CAP_CHECK,
 * inline logic) live directly in dispatch.c and are NOT in this table.
 */

#ifndef SYSCALL_TABLE_H
#define SYSCALL_TABLE_H

#define SYSCALL_TABLE(X) \
    /* I/O */ \
    X(  0, read,              3, do_read((int)a1, (void *)a2, (size_t)a3)) \
    X(  1, write,             3, do_write((int)a1, (const void *)a2, (size_t)a3)) \
    X( 17, pread64,           4, do_pread64((int)a1, (void *)a2, (size_t)a3, (int64_t)a4)) \
    X( 18, pwrite64,          4, do_pwrite64((int)a1, (const void *)a2, (size_t)a3, (int64_t)a4)) \
    X( 20, writev,            3, do_writev((int)a1, (const struct iovec *)a2, (int)a3)) \
    X(  3, close,             1, do_close((int)a1)) \
    /* Memory */ \
    X( 12, brk,               1, do_brk((unsigned long)a1)) \
    X(  9, mmap,              6, do_mmap((unsigned long)a1, (size_t)a2, (int)a3, (int)a4, (int)a5, a6)) \
    X( 11, munmap,            2, do_munmap((unsigned long)a1, (size_t)a2)) \
    X( 10, mprotect,          3, do_mprotect((unsigned long)a1, (size_t)a2, (int)a3)) \
    X(149, mlock,             2, do_mlock((unsigned long)a1, (size_t)a2)) \
    X(150, munlock,           2, do_munlock((unsigned long)a1, (size_t)a2)) \
    X(151, mlockall,          1, do_mlockall((int)a1)) \
    X(152, munlockall,        0, do_munlockall()) \
    X( 25, mremap,            5, do_mremap((unsigned long)a1, (size_t)a2, (size_t)a3, (int)a4, (unsigned long)a5)) \
    X( 28, madvise,           3, do_madvise((unsigned long)a1, (size_t)a2, (int)a3)) \
    /* Process lifecycle */ \
    X( 56, clone,             5, do_clone((unsigned long)a1, (void *)a2, (int *)a3, (int *)a4, (unsigned long)a5)) \
    X( 57, fork,              0, do_fork()) \
    X( 58, vfork,             0, do_fork()) \
    X( 59, execve,            3, do_execve((const char *)a1, (char *const *)a2, (char *const *)a3)) \
    X( 61, wait4,             4, do_wait4((int)a1, (int *)a2, (int)a3, (void *)a4)) \
    X(435, clone3,            2, do_clone3((void *)a1, (size_t)a2)) \
    /* Thread/TLS */ \
    X(158, arch_prctl,        2, do_arch_prctl((int)a1, (unsigned long)a2)) \
    X(273, set_robust_list,   0, 0) \
    /* Signals */ \
    X( 13, rt_sigaction,      4, do_rt_sigaction((int)a1, (const void *)a2, (void *)a3, (size_t)a4)) \
    X( 14, rt_sigprocmask,    4, do_rt_sigprocmask((int)a1, (const uint64_t *)a2, (uint64_t *)a3, (size_t)a4)) \
    X( 15, rt_sigreturn,      0, do_rt_sigreturn()) \
    X( 62, kill,              2, do_kill((int)a1, (int)a2)) \
    X(131, sigaltstack,       2, do_sigaltstack((const void *)a1, (void *)a2)) \
    X(130, rt_sigsuspend,     2, do_rt_sigsuspend((const uint64_t *)a1, (size_t)a2)) \
    X(234, tgkill,            3, do_tgkill((int)a1, (int)a2, (int)a3)) \
    /* Identity (single-user: uid/gid always 0) */ \
    X(102, getuid,            0, 0) \
    X(104, getgid,            0, 0) \
    X(107, geteuid,           0, 0) \
    X(108, getegid,           0, 0) \
    /* Process info */ \
    X(157, prctl,             5, do_prctl((int)a1, (unsigned long)a2, (unsigned long)a3, (unsigned long)a4, (unsigned long)a5)) \
    X( 97, getrlimit,         2, do_prlimit64(0, (int)a1, 0, (void *)a2)) \
    /* Stubs */ \
    X( 73, flock,             2, (long)0) /* advisory locks: noop, single-user */ \
    X(165, mount,             0, 0) \
    X(170, sethostname,       0, 0) \
    X(334, rseq,              0, -ENOSYS) \
    X(125, capget,            0, -EPERM) \
    X(126, capset,            0, -EPERM) \
    /* Filesystem (stat wrappers) */ \
    X(137, statfs,            2, do_statfs((const char *)a1, (void *)a2)) \
    X(138, fstatfs,           2, do_fstatfs((int)a1, (void *)a2)) \
    X(269, faccessat,         4, do_faccessat((int)a1, (const char *)a2, (int)a3, (int)a4)) \
    X(332, statx,             5, do_statx((int)a1, (const char *)a2, (int)a3, (unsigned int)a4, (void *)a5)) \
    X(267, readlinkat,        4, do_readlinkat((int)a1, (const char *)a2, (char *)a3, (size_t)a4)) \
    /* System info */ \
    X( 63, uname,             1, do_uname((void *)a1)) \
    X(318, getrandom,         3, do_getrandom((void *)a1, (size_t)a2, (unsigned int)a3)) \
    X(302, prlimit64,         4, do_prlimit64((int)a1, (int)a2, (const void *)a3, (void *)a4)) \
        X( 95, umask,             1, (long)0022) /* single-user: return default, ignore new */ \
    X( 99, sysinfo,           1, do_sysinfo((void *)a1)) \
    X(115, getgroups,         2, (long)0) \
    X(116, setgroups,         2, (long)0) \
    /* Stubs for npm/node compatibility */ \
    X( 26, msync,             3, (long)0) /* no-op: pages always coherent */ \
    X( 40, sendfile,          4, (long)-ENOSYS) /* TODO: implement if needed */ \
    X( 94, lchown,            3, (long)0) /* single-user: noop */ \
    X(146, sched_get_priority_max, 1, (long)31) \
    X(147, sched_get_priority_min, 1, (long)0) \
    X(160, setrlimit,         2, (long)0) /* noop: no enforcement */ \
    X(221, fadvise64,         4, (long)0) /* no-op: no page cache hints */ \
    X(309, getcpu,            3, do_getcpu((unsigned *)a1, (unsigned *)a2)) \
    X( 98, getrusage,         2, do_getrusage((int)a1, (void *)a2)) \
    X(100, times,             1, do_times((void *)a1)) \
    /* Timers / clocks */ \
    X(228, clock_gettime,     2, do_clock_gettime((int)a1, (void *)a2)) \
    X(229, clock_getres,      2, do_clock_getres((int)a1, (void *)a2)) \
    X(230, clock_nanosleep,   4, do_clock_nanosleep((int)a1, (int)a2, (const void *)a3, (void *)a4)) \
    X( 35, nanosleep,         2, do_nanosleep((const void *)a1, (void *)a2)) \
    X( 96, gettimeofday,      2, do_gettimeofday((void *)a1, (void *)a2)) \
    /* Scheduling */ \
    X(203, sched_setaffinity, 3, do_sched_setaffinity((int)a1, (size_t)a2, (const uint64_t *)a3)) \
    X(204, sched_getaffinity, 3, do_sched_getaffinity((int)a1, (size_t)a2, (uint64_t *)a3)) \
    X( 24, sched_yield,       0, do_sched_yield()) \
    X(144, sched_setscheduler,3, do_sched_setscheduler((int)a1, (int)a2, (const void *)a3)) \
    X(145, sched_getscheduler,1, do_sched_getscheduler((int)a1)) \
    X(142, sched_setparam,    2, do_sched_setparam((int)a1, (const void *)a2)) \
    X(143, sched_getparam,    2, do_sched_getparam((int)a1, (void *)a2)) \
    /* Filesystem */ \
    X(  2, open,              3, do_open((const char *)a1, (int)a2, (int)a3)) \
    X(257, openat,            4, do_openat((int)a1, (const char *)a2, (int)a3, (int)a4)) \
    X(  8, lseek,             3, do_lseek((int)a1, a2, (int)a3)) \
    X(  5, fstat,             2, do_fstat((int)a1, (struct k_stat *)a2)) \
    X(  4, stat,              2, do_fstatat(AT_FDCWD, (const char *)a1, (struct k_stat *)a2, 0)) \
    X(  6, lstat,             2, do_fstatat(AT_FDCWD, (const char *)a1, (struct k_stat *)a2, AT_SYMLINK_NOFOLLOW)) \
    X(262, fstatat,           4, do_fstatat((int)a1, (const char *)a2, (struct k_stat *)a3, (int)a4)) \
    X(292, dup3,              3, do_dup3((int)a1, (int)a2, (int)a3)) \
    X( 79, getcwd,            2, do_getcwd((char *)a1, (size_t)a2)) \
    X( 80, chdir,             1, do_chdir((const char *)a1)) \
    /* Network / sockets */ \
    X( 41, socket,            3, do_socket((int)a1, (int)a2, (int)a3)) \
    X( 42, connect,           3, do_connect((int)a1, (const void *)a2, (int)a3)) \
    X( 49, bind,              3, do_bind((int)a1, (const void *)a2, (int)a3)) \
    X( 50, listen,            2, do_listen((int)a1, (int)a2)) \
    X( 43, accept,            3, do_accept((int)a1, (void *)a2, (int *)a3)) \
    X( 44, sendto,            6, do_sendto((int)a1, (const void *)a2, a3, (int)a4, (const void *)a5, (int)a6)) \
    X( 45, recvfrom,          6, do_recvfrom((int)a1, (void *)a2, a3, (int)a4, (void *)a5, (int *)a6)) \
    X( 54, setsockopt,        5, do_setsockopt((int)a1, (int)a2, (int)a3, (const void *)a4, (int)a5)) \
    X( 55, getsockopt,        5, do_getsockopt((int)a1, (int)a2, (int)a3, (void *)a4, (int *)a5)) \
    X( 51, getsockname,       3, do_getsockname((int)a1, (void *)a2, (int *)a3)) \
    X( 52, getpeername,       3, do_getpeername((int)a1, (void *)a2, (int *)a3)) \
    /* sendmsg(46)/recvmsg(47): inline in dispatch.c (unix vs inet) */ \
    X( 48, shutdown,          2, do_shutdown((int)a1, (int)a2)) \
    X(  7, poll,              3, do_poll((void *)a1, (int)a2, (int)a3)) \
    /* Filesystem mutation */ \
    X( 83, mkdir,             2, do_mkdirat(AT_FDCWD, (const char *)a1, (int)a2)) \
    X(258, mkdirat,           3, do_mkdirat((int)a1, (const char *)a2, (int)a3)) \
    X( 84, rmdir,             1, do_unlinkat(AT_FDCWD, (const char *)a1, AT_REMOVEDIR)) \
    X( 87, unlink,            1, do_unlinkat(AT_FDCWD, (const char *)a1, 0)) \
    X(263, unlinkat,          3, do_unlinkat((int)a1, (const char *)a2, (int)a3)) \
    X( 82, rename,            2, do_renameat2(AT_FDCWD, (const char *)a1, AT_FDCWD, (const char *)a2, 0)) \
    X(316, renameat2,         5, do_renameat2((int)a1, (const char *)a2, (int)a3, (const char *)a4, (int)a5)) \
    X(217, getdents64,        3, do_getdents64((int)a1, (void *)a2, (size_t)a3)) \
    /* Filesystem metadata */ \
    X( 91, fchmod,            2, do_fchmod((int)a1, (uint32_t)a2)) \
    X( 93, fchown,            3, do_fchown((int)a1, (uint32_t)a2, (uint32_t)a3)) \
    X( 86, link,              2, do_linkat(AT_FDCWD, (const char *)a1, AT_FDCWD, (const char *)a2, 0)) \
    X(265, linkat,            5, do_linkat((int)a1, (const char *)a2, (int)a3, (const char *)a4, (int)a5)) \
    X( 88, symlink,           2, do_symlinkat((const char *)a1, AT_FDCWD, (const char *)a2)) \
    X(266, symlinkat,         3, do_symlinkat((const char *)a1, (int)a2, (const char *)a3)) \
    X( 89, readlink,          3, do_readlinkat(AT_FDCWD, (const char *)a1, (char *)a2, (size_t)a3)) \
    X( 76, truncate,          2, do_truncate((const char *)a1, (int64_t)a2)) \
    X( 77, ftruncate,         2, do_ftruncate((int)a1, (int64_t)a2)) \
    X( 90, chmod,             2, do_fchmodat(AT_FDCWD, (const char *)a1, (uint32_t)a2, 0)) \
    X(268, fchmodat,          4, do_fchmodat((int)a1, (const char *)a2, (uint32_t)a3, (int)a4)) \
    X(280, utimensat,         4, do_utimensat((int)a1, (const char *)a2, (const void *)a3, (int)a4)) \
    X(285, fallocate,         4, do_fallocate((int)a1, (int)a2, (int64_t)a3, (int64_t)a4)) \
    X(259, mknodat,           4, do_mknodat((int)a1, (const char *)a2, (uint32_t)a3, (uint64_t)a4)) \
    /* Pipe / IO */ \
    X( 22, pipe,              1, do_pipe2((int *)a1, 0)) \
    X(293, pipe2,             2, do_pipe2((int *)a1, (int)a2)) \
    X( 19, readv,             3, do_readv((int)a1, (const struct iovec *)a2, (int)a3)) \
    X( 16, ioctl,             3, do_ioctl((int)a1, (unsigned long)a2, (unsigned long)a3)) \
    X( 72, fcntl,             3, do_fcntl((int)a1, (int)a2, a3)) \
    X( 21, access,            2, do_faccessat(AT_FDCWD, (const char *)a1, (int)a2, 0)) \
    /* epoll / eventfd / timerfd / signalfd / inotify */ \
    X(291, epoll_create1,     1, do_epoll_create1((int)a1)) \
    X(233, epoll_ctl,         4, do_epoll_ctl((int)a1, (int)a2, (int)a3, (struct epoll_event *)a4)) \
    X(232, epoll_wait,        4, do_epoll_wait((int)a1, (struct epoll_event *)a2, (int)a3, (int)a4)) \
    X(290, eventfd2,          2, do_eventfd2((unsigned int)a1, (int)a2)) \
    X(283, timerfd_create,    2, do_timerfd_create((int)a1, (int)a2)) \
    X(286, timerfd_settime,   4, do_timerfd_settime((int)a1, (int)a2, (const struct k_itimerspec *)a3, (struct k_itimerspec *)a4)) \
    X(289, signalfd4,         3, do_signalfd4((int)a1, (const uint64_t *)a2, (int)a3)) \
    X(294, inotify_init1,     1, do_inotify_init1((int)a1)) \
    X(254, inotify_add_watch, 3, do_inotify_add_watch((int)a1, (const char *)a2, (uint32_t)a3)) \
    X(255, inotify_rm_watch,  2, do_inotify_rm_watch((int)a1, (int)a2)) \
    X(281, epoll_pwait,       4, do_epoll_wait((int)a1, (struct epoll_event *)a2, (int)a3, (int)a4)) \
    /* end */

/* Syscall name lookup for tracing */
static inline const char *syscall_name(long num) {
    switch (num) {
#define X_NAME(nr, name, nargs, handler) case nr: return #name;
    SYSCALL_TABLE(X_NAME)
#undef X_NAME
    default: return "unknown";
    }
}

/* Total number of table entries (not max syscall number) */
enum {
#define X_COUNT(nr, name, nargs, handler) _sc_##name,
    SYSCALL_TABLE(X_COUNT)
#undef X_COUNT
    SYSCALL_TABLE_COUNT
};

#endif
