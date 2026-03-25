/* CosmoRT Syscall Table — single source of truth.
 *
 * X(nr, name, handler)
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
    X(SYS_READ, read, do_read((int)a1, (void *)a2, (size_t)a3)) \
    X(SYS_WRITE, write, do_write((int)a1, (const void *)a2, (size_t)a3)) \
    X(SYS_PREAD64, pread64, do_pread64((int)a1, (void *)a2, (size_t)a3, (int64_t)a4)) \
    X(SYS_PWRITE64, pwrite64, do_pwrite64((int)a1, (const void *)a2, (size_t)a3, (int64_t)a4)) \
    X(SYS_WRITEV, writev, do_writev((int)a1, (const struct iovec *)a2, (int)a3)) \
    X(SYS_CLOSE, close, do_close((int)a1)) \
    /* Memory */ \
    X(SYS_BRK, brk, do_brk((unsigned long)a1)) \
    X(SYS_MMAP, mmap, do_mmap((unsigned long)a1, (size_t)a2, (int)a3, (int)a4, (int)a5, a6)) \
    X(SYS_MUNMAP, munmap, do_munmap((unsigned long)a1, (size_t)a2)) \
    X(SYS_MPROTECT, mprotect, do_mprotect((unsigned long)a1, (size_t)a2, (int)a3)) \
    X(SYS_MLOCK, mlock, do_mlock((unsigned long)a1, (size_t)a2)) \
    X(SYS_MUNLOCK, munlock, do_munlock((unsigned long)a1, (size_t)a2)) \
    X(SYS_MLOCKALL, mlockall, do_mlockall((int)a1)) \
    X(SYS_MUNLOCKALL, munlockall, do_munlockall()) \
    X(SYS_MREMAP, mremap, do_mremap((unsigned long)a1, (size_t)a2, (size_t)a3, (int)a4, (unsigned long)a5)) \
    X(SYS_MADVISE, madvise, do_madvise((unsigned long)a1, (size_t)a2, (int)a3)) \
    /* Process lifecycle */ \
    X(SYS_CLONE, clone, do_clone((unsigned long)a1, (void *)a2, (int *)a3, (int *)a4, (unsigned long)a5)) \
    X(SYS_FORK, fork, do_fork()) \
    X(SYS_VFORK, vfork, do_fork()) \
    X(SYS_EXECVE, execve, do_execve((const char *)a1, (char *const *)a2, (char *const *)a3)) \
    X(SYS_WAIT4, wait4, do_wait4((int)a1, (int *)a2, (int)a3, (void *)a4)) \
    X(SYS_CLONE3, clone3, do_clone3((void *)a1, (size_t)a2)) \
    /* Thread/TLS */ \
    X(SYS_ARCH_PRCTL, arch_prctl, do_arch_prctl((int)a1, (unsigned long)a2)) \
    X(SYS_SET_ROBUST_LIST, set_robust_list, do_set_robust_list()) \
    /* Signals */ \
    X(SYS_RT_SIGACTION, rt_sigaction, do_rt_sigaction((int)a1, (const void *)a2, (void *)a3, (size_t)a4)) \
    X(SYS_RT_SIGPROCMASK, rt_sigprocmask, do_rt_sigprocmask((int)a1, (const uint64_t *)a2, (uint64_t *)a3, (size_t)a4)) \
    X(SYS_RT_SIGRETURN, rt_sigreturn, do_rt_sigreturn()) \
    X(SYS_KILL, kill, do_kill((int)a1, (int)a2)) \
    X(SYS_SIGALTSTACK, sigaltstack, do_sigaltstack((const void *)a1, (void *)a2)) \
    X(SYS_RT_SIGSUSPEND, rt_sigsuspend, do_rt_sigsuspend((const uint64_t *)a1, (size_t)a2)) \
    X(SYS_TGKILL, tgkill, do_tgkill((int)a1, (int)a2, (int)a3)) \
    /* Identity (single-user: uid/gid always 0) */ \
    X(SYS_GETUID, getuid, do_getuid()) \
    X(SYS_GETGID, getgid, do_getgid()) \
    X(SYS_GETEUID, geteuid, do_geteuid()) \
    X(SYS_GETEGID, getegid, do_getegid()) \
    /* Process info */ \
    X(SYS_PRCTL, prctl, do_prctl((int)a1, (unsigned long)a2, (unsigned long)a3, (unsigned long)a4, (unsigned long)a5)) \
    X(SYS_GETRLIMIT, getrlimit, do_prlimit64(0, (int)a1, 0, (void *)a2)) \
    /* Stubs */ \
    X(SYS_FLOCK, flock, do_flock((int)a1, (int)a2)) \
    X(SYS_REBOOT, reboot, do_reboot((int)a1, (int)a2, (int)a3)) \
    X(SYS_MOUNT, mount, do_mount()) \
    X(SYS_SETHOSTNAME, sethostname, do_sethostname()) \
    X(SYS_RSEQ, rseq, do_rseq()) \
    X(SYS_CAPGET, capget, do_capget()) \
    X(SYS_CAPSET, capset, do_capset()) \
    /* Filesystem (stat wrappers) */ \
    X(SYS_STATFS, statfs, do_statfs((const char *)a1, (void *)a2)) \
    X(SYS_FSTATFS, fstatfs, do_fstatfs((int)a1, (void *)a2)) \
    X(SYS_FACCESSAT, faccessat, do_faccessat((int)a1, (const char *)a2, (int)a3, (int)a4)) \
    X(SYS_STATX, statx, do_statx((int)a1, (const char *)a2, (int)a3, (unsigned int)a4, (void *)a5)) \
    X(SYS_READLINKAT, readlinkat, do_readlinkat((int)a1, (const char *)a2, (char *)a3, (size_t)a4)) \
    /* System info */ \
    X(SYS_UNAME, uname, do_uname((void *)a1)) \
    X(SYS_GETRANDOM, getrandom, do_getrandom((void *)a1, (size_t)a2, (unsigned int)a3)) \
    X(SYS_PRLIMIT64, prlimit64, do_prlimit64((int)a1, (int)a2, (const void *)a3, (void *)a4)) \
    X(SYS_UMASK, umask, do_umask((int)a1)) \
    X(SYS_SYSINFO, sysinfo, do_sysinfo((void *)a1)) \
    X(SYS_GETGROUPS, getgroups, do_getgroups()) \
    X(SYS_SETGROUPS, setgroups, do_setgroups()) \
    /* Stubs for npm/node compatibility */ \
    X(SYS_MSYNC, msync, do_msync()) \
    X(SYS_SENDFILE, sendfile, do_sendfile()) \
    X(SYS_LCHOWN, lchown, do_lchown()) \
    X(SYS_SCHED_GET_PRIORITY_MAX, sched_get_priority_max, do_sched_get_priority_max((int)a1)) \
    X(SYS_SCHED_GET_PRIORITY_MIN, sched_get_priority_min, do_sched_get_priority_min((int)a1)) \
    X(SYS_SETRLIMIT, setrlimit, do_setrlimit()) \
    X(SYS_FADVISE64, fadvise64, do_fadvise64()) \
    X(SYS_GETCPU, getcpu, do_getcpu((unsigned *)a1, (unsigned *)a2)) \
    X(SYS_GETRUSAGE, getrusage, do_getrusage((int)a1, (void *)a2)) \
    X(SYS_TIMES, times, do_times((void *)a1)) \
    /* Timers / clocks */ \
    X(SYS_CLOCK_GETTIME, clock_gettime, do_clock_gettime((int)a1, (void *)a2)) \
    X(SYS_CLOCK_GETRES, clock_getres, do_clock_getres((int)a1, (void *)a2)) \
    X(SYS_CLOCK_NANOSLEEP, clock_nanosleep, do_clock_nanosleep((int)a1, (int)a2, (const void *)a3, (void *)a4)) \
    X(SYS_NANOSLEEP, nanosleep, do_nanosleep((const void *)a1, (void *)a2)) \
    X(SYS_GETTIMEOFDAY, gettimeofday, do_gettimeofday((void *)a1, (void *)a2)) \
    /* Scheduling */ \
    X(SYS_SCHED_SETAFFINITY, sched_setaffinity, do_sched_setaffinity((int)a1, (size_t)a2, (const uint64_t *)a3)) \
    X(SYS_SCHED_GETAFFINITY, sched_getaffinity, do_sched_getaffinity((int)a1, (size_t)a2, (uint64_t *)a3)) \
    X(SYS_SCHED_YIELD, sched_yield, do_sched_yield()) \
    X(SYS_SCHED_SETSCHEDULER, sched_setscheduler, do_sched_setscheduler((int)a1, (int)a2, (const void *)a3)) \
    X(SYS_SCHED_GETSCHEDULER, sched_getscheduler, do_sched_getscheduler((int)a1)) \
    X(SYS_SCHED_SETPARAM, sched_setparam, do_sched_setparam((int)a1, (const void *)a2)) \
    X(SYS_SCHED_GETPARAM, sched_getparam, do_sched_getparam((int)a1, (void *)a2)) \
    /* Filesystem */ \
    X(SYS_OPEN, open, do_open((const char *)a1, (int)a2, (int)a3)) \
    X(SYS_OPENAT, openat, do_openat((int)a1, (const char *)a2, (int)a3, (int)a4)) \
    X(SYS_LSEEK, lseek, do_lseek((int)a1, a2, (int)a3)) \
    X(SYS_FSTAT, fstat, do_fstat((int)a1, (struct k_stat *)a2)) \
    X(SYS_STAT, stat, do_fstatat(AT_FDCWD, (const char *)a1, (struct k_stat *)a2, 0)) \
    X(SYS_LSTAT, lstat, do_fstatat(AT_FDCWD, (const char *)a1, (struct k_stat *)a2, AT_SYMLINK_NOFOLLOW)) \
    X(SYS_FSTATAT, fstatat, do_fstatat((int)a1, (const char *)a2, (struct k_stat *)a3, (int)a4)) \
    X(SYS_DUP3, dup3, do_dup3((int)a1, (int)a2, (int)a3)) \
    X(SYS_GETCWD, getcwd, do_getcwd((char *)a1, (size_t)a2)) \
    X(SYS_CHDIR, chdir, do_chdir((const char *)a1)) \
    /* Network / sockets */ \
    X(SYS_SOCKET, socket, do_socket((int)a1, (int)a2, (int)a3)) \
    X(SYS_CONNECT, connect, do_connect((int)a1, (const void *)a2, (int)a3)) \
    X(SYS_BIND, bind, do_bind((int)a1, (const void *)a2, (int)a3)) \
    X(SYS_LISTEN, listen, do_listen((int)a1, (int)a2)) \
    X(SYS_ACCEPT, accept, do_accept((int)a1, (void *)a2, (int *)a3)) \
    X(SYS_SENDTO, sendto, do_sendto((int)a1, (const void *)a2, a3, (int)a4, (const void *)a5, (int)a6)) \
    X(SYS_RECVFROM, recvfrom, do_recvfrom((int)a1, (void *)a2, a3, (int)a4, (void *)a5, (int *)a6)) \
    X(SYS_SETSOCKOPT, setsockopt, do_setsockopt((int)a1, (int)a2, (int)a3, (const void *)a4, (int)a5)) \
    X(SYS_GETSOCKOPT, getsockopt, do_getsockopt((int)a1, (int)a2, (int)a3, (void *)a4, (int *)a5)) \
    X(SYS_GETSOCKNAME, getsockname, do_getsockname((int)a1, (void *)a2, (int *)a3)) \
    X(SYS_GETPEERNAME, getpeername, do_getpeername((int)a1, (void *)a2, (int *)a3)) \
    /* sendmsg(46)/recvmsg(47): in sys_net.c */ \
    X(SYS_SENDMSG, sendmsg, do_sendmsg((int)a1, (const void *)a2, (int)a3)) \
    X(SYS_RECVMSG, recvmsg, do_recvmsg((int)a1, (void *)a2, (int)a3)) \
    X(SYS_SHUTDOWN, shutdown, do_shutdown((int)a1, (int)a2)) \
    X(SYS_POLL, poll, do_poll((void *)a1, (int)a2, (int)a3)) \
    /* Filesystem mutation */ \
    X(SYS_MKDIR, mkdir, do_mkdirat(AT_FDCWD, (const char *)a1, (int)a2)) \
    X(SYS_MKDIRAT, mkdirat, do_mkdirat((int)a1, (const char *)a2, (int)a3)) \
    X(SYS_RMDIR, rmdir, do_unlinkat(AT_FDCWD, (const char *)a1, AT_REMOVEDIR)) \
    X(SYS_UNLINK, unlink, do_unlinkat(AT_FDCWD, (const char *)a1, 0)) \
    X(SYS_UNLINKAT, unlinkat, do_unlinkat((int)a1, (const char *)a2, (int)a3)) \
    X(SYS_RENAME, rename, do_renameat2(AT_FDCWD, (const char *)a1, AT_FDCWD, (const char *)a2, 0)) \
    X(SYS_RENAMEAT2, renameat2, do_renameat2((int)a1, (const char *)a2, (int)a3, (const char *)a4, (int)a5)) \
    X(SYS_GETDENTS64, getdents64, do_getdents64((int)a1, (void *)a2, (size_t)a3)) \
    /* Filesystem metadata */ \
    X(SYS_FCHMOD, fchmod, do_fchmod((int)a1, (uint32_t)a2)) \
    X(SYS_FCHOWN, fchown, do_fchown((int)a1, (uint32_t)a2, (uint32_t)a3)) \
    X(SYS_LINK, link, do_linkat(AT_FDCWD, (const char *)a1, AT_FDCWD, (const char *)a2, 0)) \
    X(SYS_LINKAT, linkat, do_linkat((int)a1, (const char *)a2, (int)a3, (const char *)a4, (int)a5)) \
    X(SYS_SYMLINK, symlink, do_symlinkat((const char *)a1, AT_FDCWD, (const char *)a2)) \
    X(SYS_SYMLINKAT, symlinkat, do_symlinkat((const char *)a1, (int)a2, (const char *)a3)) \
    X(SYS_READLINK, readlink, do_readlinkat(AT_FDCWD, (const char *)a1, (char *)a2, (size_t)a3)) \
    X(SYS_TRUNCATE, truncate, do_truncate((const char *)a1, (int64_t)a2)) \
    X(SYS_FTRUNCATE, ftruncate, do_ftruncate((int)a1, (int64_t)a2)) \
    X(SYS_CHMOD, chmod, do_fchmodat(AT_FDCWD, (const char *)a1, (uint32_t)a2, 0)) \
    X(SYS_FCHMODAT, fchmodat, do_fchmodat((int)a1, (const char *)a2, (uint32_t)a3, (int)a4)) \
    X(SYS_UTIMENSAT, utimensat, do_utimensat((int)a1, (const char *)a2, (const void *)a3, (int)a4)) \
    X(SYS_FALLOCATE, fallocate, do_fallocate((int)a1, (int)a2, (int64_t)a3, (int64_t)a4)) \
    X(SYS_MKNODAT, mknodat, do_mknodat((int)a1, (const char *)a2, (uint32_t)a3, (uint64_t)a4)) \
    /* Pipe / IO */ \
    X(SYS_PIPE, pipe, do_pipe2((int *)a1, 0)) \
    X(SYS_PIPE2, pipe2, do_pipe2((int *)a1, (int)a2)) \
    X(SYS_READV, readv, do_readv((int)a1, (const struct iovec *)a2, (int)a3)) \
    X(SYS_IOCTL, ioctl, do_ioctl((int)a1, (unsigned long)a2, (unsigned long)a3)) \
    X(SYS_FCNTL, fcntl, do_fcntl((int)a1, (int)a2, a3)) \
    X(SYS_ACCESS, access, do_faccessat(AT_FDCWD, (const char *)a1, (int)a2, 0)) \
    /* epoll / eventfd / timerfd / signalfd / inotify */ \
    X(SYS_EPOLL_CREATE1, epoll_create1, do_epoll_create1((int)a1)) \
    X(SYS_EPOLL_CTL, epoll_ctl, do_epoll_ctl((int)a1, (int)a2, (int)a3, (struct epoll_event *)a4)) \
    X(SYS_EPOLL_WAIT, epoll_wait, do_epoll_wait((int)a1, (struct epoll_event *)a2, (int)a3, (int)a4)) \
    X(SYS_EVENTFD2, eventfd2, do_eventfd2((unsigned int)a1, (int)a2)) \
    X(SYS_TIMERFD_CREATE, timerfd_create, do_timerfd_create((int)a1, (int)a2)) \
    X(SYS_TIMERFD_SETTIME, timerfd_settime, do_timerfd_settime((int)a1, (int)a2, (const struct k_itimerspec *)a3, (struct k_itimerspec *)a4)) \
    X(SYS_SIGNALFD4, signalfd4, do_signalfd4((int)a1, (const uint64_t *)a2, (int)a3)) \
    X(SYS_INOTIFY_INIT1, inotify_init1, do_inotify_init1((int)a1)) \
    X(SYS_INOTIFY_ADD_WATCH, inotify_add_watch, do_inotify_add_watch((int)a1, (const char *)a2, (uint32_t)a3)) \
    X(SYS_INOTIFY_RM_WATCH, inotify_rm_watch, do_inotify_rm_watch((int)a1, (int)a2)) \
    X(SYS_EPOLL_PWAIT, epoll_pwait, do_epoll_wait((int)a1, (struct epoll_event *)a2, (int)a3, (int)a4)) \
    /* select / pselect / ppoll */ \
    X(SYS_SELECT, select, do_pselect6((int)a1, (uint64_t *)a2, a3, a4, a5, SYS_SELECT)) \
    X(SYS_PSELECT6, pselect6, do_pselect6((int)a1, (uint64_t *)a2, a3, a4, a5, SYS_PSELECT6)) \
    X(SYS_PPOLL, ppoll, do_ppoll(a1, a2, a3)) \
    /* vectored I/O with offset */ \
    X(SYS_PREADV, preadv, do_readv((int)a1, (const struct iovec *)a2, (int)a3)) \
    X(SYS_PWRITEV, pwritev, do_writev((int)a1, (const struct iovec *)a2, (int)a3)) \
    /* multi-message send/recv */ \
    X(SYS_SENDMMSG, sendmmsg, do_sendmmsg((int)a1, (uint64_t)a2, (int)a3, (int)a4)) \
    X(SYS_RECVMMSG, recvmmsg, do_recvmmsg((int)a1, (uint64_t)a2, (int)a3, (int)a4)) \
    /* end */

/* Syscall name lookup for tracing */
static inline const char *syscall_name(long num) {
    switch (num) {
#define X_NAME(nr, name, handler) case nr: return #name;
    SYSCALL_TABLE(X_NAME)
#undef X_NAME
    default: return "unknown";
    }
}

/* Total number of table entries (not max syscall number) */
enum {
#define X_COUNT(nr, name, handler) _sc_##name,
    SYSCALL_TABLE(X_COUNT)
#undef X_COUNT
    SYSCALL_TABLE_COUNT
};

#endif
