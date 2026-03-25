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

#include "linux/syscall.h"

#define SYSCALL_TABLE(X) \
    /* I/O */ \
    X(SYS_READ, read,              3, do_read((int)a1, (void *)a2, (size_t)a3)) \
    X(SYS_WRITE, write,            3, do_write((int)a1, (const void *)a2, (size_t)a3)) \
    X(SYS_PREAD64, pread64,        4, do_pread64((int)a1, (void *)a2, (size_t)a3, (int64_t)a4)) \
    X(SYS_PWRITE64, pwrite64,      4, do_pwrite64((int)a1, (const void *)a2, (size_t)a3, (int64_t)a4)) \
    X(SYS_WRITEV, writev,          3, do_writev((int)a1, (const struct iovec *)a2, (int)a3)) \
    X(SYS_CLOSE, close,            1, do_close((int)a1)) \
    /* Memory */ \
    X(SYS_BRK, brk,               1, do_brk((unsigned long)a1)) \
    X(SYS_MMAP, mmap,             6, do_mmap((unsigned long)a1, (size_t)a2, (int)a3, (int)a4, (int)a5, a6)) \
    X(SYS_MUNMAP, munmap,          2, do_munmap((unsigned long)a1, (size_t)a2)) \
    X(SYS_MPROTECT, mprotect,     3, do_mprotect((unsigned long)a1, (size_t)a2, (int)a3)) \
    X(SYS_MLOCK, mlock,           2, do_mlock((unsigned long)a1, (size_t)a2)) \
    X(SYS_MUNLOCK, munlock,        2, do_munlock((unsigned long)a1, (size_t)a2)) \
    X(SYS_MLOCKALL, mlockall,     1, do_mlockall((int)a1)) \
    X(SYS_MUNLOCKALL, munlockall, 0, do_munlockall()) \
    X(SYS_MREMAP, mremap,         5, do_mremap((unsigned long)a1, (size_t)a2, (size_t)a3, (int)a4, (unsigned long)a5)) \
    X(SYS_MADVISE, madvise,       3, do_madvise((unsigned long)a1, (size_t)a2, (int)a3)) \
    /* Process lifecycle */ \
    X(SYS_CLONE, clone,           5, do_clone((unsigned long)a1, (void *)a2, (int *)a3, (int *)a4, (unsigned long)a5)) \
    X(SYS_FORK, fork,             0, do_fork()) \
    X(SYS_VFORK, vfork,           0, do_fork()) \
    X(SYS_EXECVE, execve,         3, do_execve((const char *)a1, (char *const *)a2, (char *const *)a3)) \
    X(SYS_WAIT4, wait4,           4, do_wait4((int)a1, (int *)a2, (int)a3, (void *)a4)) \
    X(SYS_CLONE3, clone3,         2, do_clone3((void *)a1, (size_t)a2)) \
    /* Thread/TLS */ \
    X(SYS_ARCH_PRCTL, arch_prctl, 2, do_arch_prctl((int)a1, (unsigned long)a2)) \
    X(SYS_SET_ROBUST_LIST, set_robust_list, 0, do_set_robust_list()) \
    /* Signals */ \
    X(SYS_RT_SIGACTION, rt_sigaction, 4, do_rt_sigaction((int)a1, (const void *)a2, (void *)a3, (size_t)a4)) \
    X(SYS_RT_SIGPROCMASK, rt_sigprocmask, 4, do_rt_sigprocmask((int)a1, (const uint64_t *)a2, (uint64_t *)a3, (size_t)a4)) \
    X(SYS_RT_SIGRETURN, rt_sigreturn, 0, do_rt_sigreturn()) \
    X(SYS_KILL, kill,             2, do_kill((int)a1, (int)a2)) \
    X(SYS_SIGALTSTACK, sigaltstack, 2, do_sigaltstack((const void *)a1, (void *)a2)) \
    X(SYS_RT_SIGSUSPEND, rt_sigsuspend, 2, do_rt_sigsuspend((const uint64_t *)a1, (size_t)a2)) \
    X(SYS_TGKILL, tgkill,        3, do_tgkill((int)a1, (int)a2, (int)a3)) \
    /* Identity (single-user: uid/gid always 0) */ \
    X(SYS_GETUID, getuid,         0, do_getuid()) \
    X(SYS_GETGID, getgid,         0, do_getgid()) \
    X(SYS_GETEUID, geteuid,       0, do_geteuid()) \
    X(SYS_GETEGID, getegid,       0, do_getegid()) \
    /* Process info */ \
    X(SYS_PRCTL, prctl,           5, do_prctl((int)a1, (unsigned long)a2, (unsigned long)a3, (unsigned long)a4, (unsigned long)a5)) \
    X(SYS_GETRLIMIT, getrlimit,   2, do_prlimit64(0, (int)a1, 0, (void *)a2)) \
    /* Stubs */ \
    X(SYS_FLOCK, flock,           2, do_flock((int)a1, (int)a2)) \
    X(SYS_REBOOT, reboot,         3, do_reboot((int)a1, (int)a2, (int)a3)) \
    X(SYS_MOUNT, mount,           0, do_mount()) \
    X(SYS_SETHOSTNAME, sethostname, 0, do_sethostname()) \
    X(SYS_RSEQ, rseq,            0, do_rseq()) \
    X(SYS_CAPGET, capget,         0, do_capget()) \
    X(SYS_CAPSET, capset,         0, do_capset()) \
    /* Filesystem (stat wrappers) */ \
    X(SYS_STATFS, statfs,         2, do_statfs((const char *)a1, (void *)a2)) \
    X(SYS_FSTATFS, fstatfs,       2, do_fstatfs((int)a1, (void *)a2)) \
    X(SYS_FACCESSAT, faccessat,   4, do_faccessat((int)a1, (const char *)a2, (int)a3, (int)a4)) \
    X(SYS_STATX, statx,           5, do_statx((int)a1, (const char *)a2, (int)a3, (unsigned int)a4, (void *)a5)) \
    X(SYS_READLINKAT, readlinkat, 4, do_readlinkat((int)a1, (const char *)a2, (char *)a3, (size_t)a4)) \
    /* System info */ \
    X(SYS_UNAME, uname,           1, do_uname((void *)a1)) \
    X(SYS_GETRANDOM, getrandom,   3, do_getrandom((void *)a1, (size_t)a2, (unsigned int)a3)) \
    X(SYS_PRLIMIT64, prlimit64,   4, do_prlimit64((int)a1, (int)a2, (const void *)a3, (void *)a4)) \
    X(SYS_UMASK, umask,           1, do_umask((int)a1)) \
    X(SYS_SYSINFO, sysinfo,       1, do_sysinfo((void *)a1)) \
    X(SYS_GETGROUPS, getgroups,   2, do_getgroups()) \
    X(SYS_SETGROUPS, setgroups,   2, do_setgroups()) \
    /* Stubs for npm/node compatibility */ \
    X(SYS_MSYNC, msync,           3, do_msync()) \
    X(SYS_SENDFILE, sendfile,     4, do_sendfile()) \
    X(SYS_LCHOWN, lchown,         3, do_lchown()) \
    X(SYS_SCHED_GET_PRIORITY_MAX, sched_get_priority_max, 1, do_sched_get_priority_max((int)a1)) \
    X(SYS_SCHED_GET_PRIORITY_MIN, sched_get_priority_min, 1, do_sched_get_priority_min((int)a1)) \
    X(SYS_SETRLIMIT, setrlimit,   2, do_setrlimit()) \
    X(SYS_FADVISE64, fadvise64,   4, do_fadvise64()) \
    X(SYS_GETCPU, getcpu,         3, do_getcpu((unsigned *)a1, (unsigned *)a2)) \
    X(SYS_GETRUSAGE, getrusage,   2, do_getrusage((int)a1, (void *)a2)) \
    X(SYS_TIMES, times,           1, do_times((void *)a1)) \
    /* Timers / clocks */ \
    X(SYS_CLOCK_GETTIME, clock_gettime, 2, do_clock_gettime((int)a1, (void *)a2)) \
    X(SYS_CLOCK_GETRES, clock_getres, 2, do_clock_getres((int)a1, (void *)a2)) \
    X(SYS_CLOCK_NANOSLEEP, clock_nanosleep, 4, do_clock_nanosleep((int)a1, (int)a2, (const void *)a3, (void *)a4)) \
    X(SYS_NANOSLEEP, nanosleep,   2, do_nanosleep((const void *)a1, (void *)a2)) \
    X(SYS_GETTIMEOFDAY, gettimeofday, 2, do_gettimeofday((void *)a1, (void *)a2)) \
    /* Scheduling */ \
    X(SYS_SCHED_SETAFFINITY, sched_setaffinity, 3, do_sched_setaffinity((int)a1, (size_t)a2, (const uint64_t *)a3)) \
    X(SYS_SCHED_GETAFFINITY, sched_getaffinity, 3, do_sched_getaffinity((int)a1, (size_t)a2, (uint64_t *)a3)) \
    X(SYS_SCHED_YIELD, sched_yield, 0, do_sched_yield()) \
    X(SYS_SCHED_SETSCHEDULER, sched_setscheduler, 3, do_sched_setscheduler((int)a1, (int)a2, (const void *)a3)) \
    X(SYS_SCHED_GETSCHEDULER, sched_getscheduler, 1, do_sched_getscheduler((int)a1)) \
    X(SYS_SCHED_SETPARAM, sched_setparam, 2, do_sched_setparam((int)a1, (const void *)a2)) \
    X(SYS_SCHED_GETPARAM, sched_getparam, 2, do_sched_getparam((int)a1, (void *)a2)) \
    /* Filesystem */ \
    X(SYS_OPEN, open,             3, do_open((const char *)a1, (int)a2, (int)a3)) \
    X(SYS_OPENAT, openat,         4, do_openat((int)a1, (const char *)a2, (int)a3, (int)a4)) \
    X(SYS_LSEEK, lseek,           3, do_lseek((int)a1, a2, (int)a3)) \
    X(SYS_FSTAT, fstat,            2, do_fstat((int)a1, (struct k_stat *)a2)) \
    X(SYS_STAT, stat,              2, do_fstatat(AT_FDCWD, (const char *)a1, (struct k_stat *)a2, 0)) \
    X(SYS_LSTAT, lstat,            2, do_fstatat(AT_FDCWD, (const char *)a1, (struct k_stat *)a2, AT_SYMLINK_NOFOLLOW)) \
    X(SYS_FSTATAT, fstatat,       4, do_fstatat((int)a1, (const char *)a2, (struct k_stat *)a3, (int)a4)) \
    X(SYS_DUP3, dup3,             3, do_dup3((int)a1, (int)a2, (int)a3)) \
    X(SYS_GETCWD, getcwd,         2, do_getcwd((char *)a1, (size_t)a2)) \
    X(SYS_CHDIR, chdir,           1, do_chdir((const char *)a1)) \
    /* Network / sockets */ \
    X(SYS_SOCKET, socket,         3, do_socket((int)a1, (int)a2, (int)a3)) \
    X(SYS_CONNECT, connect,       3, do_connect((int)a1, (const void *)a2, (int)a3)) \
    X(SYS_BIND, bind,             3, do_bind((int)a1, (const void *)a2, (int)a3)) \
    X(SYS_LISTEN, listen,         2, do_listen((int)a1, (int)a2)) \
    X(SYS_ACCEPT, accept,         3, do_accept((int)a1, (void *)a2, (int *)a3)) \
    X(SYS_SENDTO, sendto,         6, do_sendto((int)a1, (const void *)a2, a3, (int)a4, (const void *)a5, (int)a6)) \
    X(SYS_RECVFROM, recvfrom,     6, do_recvfrom((int)a1, (void *)a2, a3, (int)a4, (void *)a5, (int *)a6)) \
    X(SYS_SETSOCKOPT, setsockopt, 5, do_setsockopt((int)a1, (int)a2, (int)a3, (const void *)a4, (int)a5)) \
    X(SYS_GETSOCKOPT, getsockopt, 5, do_getsockopt((int)a1, (int)a2, (int)a3, (void *)a4, (int *)a5)) \
    X(SYS_GETSOCKNAME, getsockname, 3, do_getsockname((int)a1, (void *)a2, (int *)a3)) \
    X(SYS_GETPEERNAME, getpeername, 3, do_getpeername((int)a1, (void *)a2, (int *)a3)) \
    /* sendmsg(46)/recvmsg(47): in sys_net.c */ \
    X(SYS_SENDMSG, sendmsg,      3, do_sendmsg((int)a1, (const void *)a2, (int)a3)) \
    X(SYS_RECVMSG, recvmsg,      3, do_recvmsg((int)a1, (void *)a2, (int)a3)) \
    X(SYS_SHUTDOWN, shutdown,     2, do_shutdown((int)a1, (int)a2)) \
    X(SYS_POLL, poll,              3, do_poll((void *)a1, (int)a2, (int)a3)) \
    /* Filesystem mutation */ \
    X(SYS_MKDIR, mkdir,            2, do_mkdirat(AT_FDCWD, (const char *)a1, (int)a2)) \
    X(SYS_MKDIRAT, mkdirat,       3, do_mkdirat((int)a1, (const char *)a2, (int)a3)) \
    X(SYS_RMDIR, rmdir,           1, do_unlinkat(AT_FDCWD, (const char *)a1, AT_REMOVEDIR)) \
    X(SYS_UNLINK, unlink,         1, do_unlinkat(AT_FDCWD, (const char *)a1, 0)) \
    X(SYS_UNLINKAT, unlinkat,     3, do_unlinkat((int)a1, (const char *)a2, (int)a3)) \
    X(SYS_RENAME, rename,         2, do_renameat2(AT_FDCWD, (const char *)a1, AT_FDCWD, (const char *)a2, 0)) \
    X(SYS_RENAMEAT2, renameat2,   5, do_renameat2((int)a1, (const char *)a2, (int)a3, (const char *)a4, (int)a5)) \
    X(SYS_GETDENTS64, getdents64, 3, do_getdents64((int)a1, (void *)a2, (size_t)a3)) \
    /* Filesystem metadata */ \
    X(SYS_FCHMOD, fchmod,         2, do_fchmod((int)a1, (uint32_t)a2)) \
    X(SYS_FCHOWN, fchown,         3, do_fchown((int)a1, (uint32_t)a2, (uint32_t)a3)) \
    X(SYS_LINK, link,              2, do_linkat(AT_FDCWD, (const char *)a1, AT_FDCWD, (const char *)a2, 0)) \
    X(SYS_LINKAT, linkat,         5, do_linkat((int)a1, (const char *)a2, (int)a3, (const char *)a4, (int)a5)) \
    X(SYS_SYMLINK, symlink,       2, do_symlinkat((const char *)a1, AT_FDCWD, (const char *)a2)) \
    X(SYS_SYMLINKAT, symlinkat,   3, do_symlinkat((const char *)a1, (int)a2, (const char *)a3)) \
    X(SYS_READLINK, readlink,     3, do_readlinkat(AT_FDCWD, (const char *)a1, (char *)a2, (size_t)a3)) \
    X(SYS_TRUNCATE, truncate,     2, do_truncate((const char *)a1, (int64_t)a2)) \
    X(SYS_FTRUNCATE, ftruncate,   2, do_ftruncate((int)a1, (int64_t)a2)) \
    X(SYS_CHMOD, chmod,            2, do_fchmodat(AT_FDCWD, (const char *)a1, (uint32_t)a2, 0)) \
    X(SYS_FCHMODAT, fchmodat,     4, do_fchmodat((int)a1, (const char *)a2, (uint32_t)a3, (int)a4)) \
    X(SYS_UTIMENSAT, utimensat,   4, do_utimensat((int)a1, (const char *)a2, (const void *)a3, (int)a4)) \
    X(SYS_FALLOCATE, fallocate,   4, do_fallocate((int)a1, (int)a2, (int64_t)a3, (int64_t)a4)) \
    X(SYS_MKNODAT, mknodat,       4, do_mknodat((int)a1, (const char *)a2, (uint32_t)a3, (uint64_t)a4)) \
    /* Pipe / IO */ \
    X(SYS_PIPE, pipe,              1, do_pipe2((int *)a1, 0)) \
    X(SYS_PIPE2, pipe2,           2, do_pipe2((int *)a1, (int)a2)) \
    X(SYS_READV, readv,           3, do_readv((int)a1, (const struct iovec *)a2, (int)a3)) \
    X(SYS_IOCTL, ioctl,           3, do_ioctl((int)a1, (unsigned long)a2, (unsigned long)a3)) \
    X(SYS_FCNTL, fcntl,           3, do_fcntl((int)a1, (int)a2, a3)) \
    X(SYS_ACCESS, access,         2, do_faccessat(AT_FDCWD, (const char *)a1, (int)a2, 0)) \
    /* epoll / eventfd / timerfd / signalfd / inotify */ \
    X(SYS_EPOLL_CREATE1, epoll_create1, 1, do_epoll_create1((int)a1)) \
    X(SYS_EPOLL_CTL, epoll_ctl,   4, do_epoll_ctl((int)a1, (int)a2, (int)a3, (struct epoll_event *)a4)) \
    X(SYS_EPOLL_WAIT, epoll_wait, 4, do_epoll_wait((int)a1, (struct epoll_event *)a2, (int)a3, (int)a4)) \
    X(SYS_EVENTFD2, eventfd2,     2, do_eventfd2((unsigned int)a1, (int)a2)) \
    X(SYS_TIMERFD_CREATE, timerfd_create, 2, do_timerfd_create((int)a1, (int)a2)) \
    X(SYS_TIMERFD_SETTIME, timerfd_settime, 4, do_timerfd_settime((int)a1, (int)a2, (const struct k_itimerspec *)a3, (struct k_itimerspec *)a4)) \
    X(SYS_SIGNALFD4, signalfd4,   3, do_signalfd4((int)a1, (const uint64_t *)a2, (int)a3)) \
    X(SYS_INOTIFY_INIT1, inotify_init1, 1, do_inotify_init1((int)a1)) \
    X(SYS_INOTIFY_ADD_WATCH, inotify_add_watch, 3, do_inotify_add_watch((int)a1, (const char *)a2, (uint32_t)a3)) \
    X(SYS_INOTIFY_RM_WATCH, inotify_rm_watch, 2, do_inotify_rm_watch((int)a1, (int)a2)) \
    X(SYS_EPOLL_PWAIT, epoll_pwait, 4, do_epoll_wait((int)a1, (struct epoll_event *)a2, (int)a3, (int)a4)) \
    /* select / pselect / ppoll */ \
    X(SYS_SELECT, select,          5, do_pselect6((int)a1, (uint64_t *)a2, a3, a4, a5, SYS_SELECT)) \
    X(SYS_PSELECT6, pselect6,      5, do_pselect6((int)a1, (uint64_t *)a2, a3, a4, a5, SYS_PSELECT6)) \
    X(SYS_PPOLL, ppoll,            3, do_ppoll(a1, a2, a3)) \
    /* vectored I/O with offset */ \
    X(SYS_PREADV, preadv,          3, do_readv((int)a1, (const struct iovec *)a2, (int)a3)) \
    X(SYS_PWRITEV, pwritev,        3, do_writev((int)a1, (const struct iovec *)a2, (int)a3)) \
    /* multi-message send/recv */ \
    X(SYS_SENDMMSG, sendmmsg,      4, do_sendmmsg((int)a1, (uint64_t)a2, (int)a3, (int)a4)) \
    X(SYS_RECVMMSG, recvmmsg,      4, do_recvmmsg((int)a1, (uint64_t)a2, (int)a3, (int)a4)) \
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
