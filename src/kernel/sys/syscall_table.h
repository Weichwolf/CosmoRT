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
    X(SYS_FORK, fork, do_fork(SIGCHLD, 0, 0, 0, 0)) \
    X(SYS_VFORK, vfork, do_vfork(CLONE_VFORK, 0, 0, 0, 0)) \
    X(SYS_EXECVE, execve, do_execve((const char *)a1, (char *const *)a2, (char *const *)a3)) \
    X(SYS_WAIT4, wait4, do_wait4((int)a1, (int *)a2, (int)a3, (void *)a4)) \
    X(SYS_CLONE3, clone3, do_clone3((void *)a1, (size_t)a2)) \
    /* Thread/TLS */ \
    X(SYS_ARCH_PRCTL, arch_prctl, do_arch_prctl((int)a1, (unsigned long)a2)) \
    X(SYS_SET_ROBUST_LIST, set_robust_list, do_set_robust_list((void *)a1, (size_t)a2)) \
    /* Signals */ \
    X(SYS_RT_SIGACTION, rt_sigaction, do_rt_sigaction((int)a1, (const void *)a2, (void *)a3, (size_t)a4)) \
    X(SYS_RT_SIGPROCMASK, rt_sigprocmask, do_rt_sigprocmask((int)a1, (const uint64_t *)a2, (uint64_t *)a3, (size_t)a4)) \
    X(SYS_RT_SIGRETURN, rt_sigreturn, do_rt_sigreturn()) \
    X(SYS_KILL, kill, do_kill((int)a1, (int)a2)) \
    X(SYS_SIGALTSTACK, sigaltstack, do_sigaltstack((const void *)a1, (void *)a2)) \
    X(SYS_RT_SIGSUSPEND, rt_sigsuspend, do_rt_sigsuspend((const uint64_t *)a1, (size_t)a2)) \
    X(SYS_TGKILL, tgkill, do_tgkill((int)a1, (int)a2, (int)a3)) \
    X(SYS_TKILL, tkill, do_tkill((int)a1, (int)a2)) \
    X(SYS_RT_SIGPENDING, rt_sigpending, do_rt_sigpending((uint64_t *)a1, (size_t)a2)) \
    X(SYS_RT_SIGTIMEDWAIT, rt_sigtimedwait, do_rt_sigtimedwait((const uint64_t *)a1, (void *)a2, (const struct k_timespec *)a3, (size_t)a4)) \
    X(SYS_RT_SIGQUEUEINFO, rt_sigqueueinfo, do_rt_sigqueueinfo((int)a1, (int)a2, (void *)a3)) \
    /* Identity (single-user: uid/gid always 0) */ \
    X(SYS_GETUID, getuid, do_getuid()) \
    X(SYS_GETGID, getgid, do_getgid()) \
    X(SYS_GETEUID, geteuid, do_geteuid()) \
    X(SYS_GETEGID, getegid, do_getegid()) \
    X(SYS_SETUID, setuid, do_setuid(a1)) \
    X(SYS_SETGID, setgid, do_setgid(a1)) \
    X(SYS_SETREUID, setreuid, do_setreuid(a1, a2)) \
    X(SYS_SETREGID, setregid, do_setregid(a1, a2)) \
    X(SYS_SETRESUID, setresuid, do_setresuid(a1, a2, a3)) \
    X(SYS_SETRESGID, setresgid, do_setresgid(a1, a2, a3)) \
    X(SYS_GETRESUID, getresuid, do_getresuid((long *)a1, (long *)a2, (long *)a3)) \
    X(SYS_GETRESGID, getresgid, do_getresgid((long *)a1, (long *)a2, (long *)a3)) \
    X(SYS_SETFSUID, setfsuid, do_setfsuid(a1)) \
    X(SYS_SETFSGID, setfsgid, do_setfsgid(a1)) \
    /* Process info */ \
    X(SYS_PRCTL, prctl, do_prctl((int)a1, (unsigned long)a2, (unsigned long)a3, (unsigned long)a4, (unsigned long)a5)) \
    X(SYS_GETRLIMIT, getrlimit, do_prlimit64(0, (int)a1, 0, (void *)a2)) \
    /* Process/timer */ \
    X(SYS_PAUSE, pause, do_pause()) \
    X(SYS_GETITIMER, getitimer, do_getitimer((int)a1, (void *)a2)) \
    X(SYS_SETITIMER, setitimer, do_setitimer((int)a1, (const void *)a2, (void *)a3)) \
    X(SYS_WAITID, waitid, do_waitid((int)a1, (int)a2, (void *)a3, (int)a4)) \
    X(SYS_PERSONALITY, personality, do_personality((unsigned long)a1)) \
    X(SYS_GETPRIORITY, getpriority, do_getpriority((int)a1, (int)a2)) \
    X(SYS_SETPRIORITY, setpriority, do_setpriority((int)a1, (int)a2, (int)a3)) \
    /* Stubs */ \
    X(SYS_FLOCK, flock, do_flock((int)a1, (int)a2)) \
    X(SYS_REBOOT, reboot, do_reboot((int)a1, (int)a2, (int)a3)) \
    X(SYS_MOUNT, mount, do_mount((const char *)a1, (const char *)a2, (const char *)a3, (unsigned long)a4, (const void *)a5)) \
    X(SYS_SETHOSTNAME, sethostname, do_sethostname()) \
    X(SYS_RSEQ, rseq, do_rseq()) \
    X(SYS_CAPGET, capget, do_capget((void *)a1, (void *)a2)) \
    X(SYS_CAPSET, capset, do_capset((void *)a1, (const void *)a2)) \
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
    X(SYS_GETGROUPS, getgroups, do_getgroups((int)a1, (uint32_t *)a2)) \
    X(SYS_SETGROUPS, setgroups, do_setgroups((int)a1, (const uint32_t *)a2)) \
    /* Stubs for npm/node compatibility */ \
    X(SYS_MSYNC, msync, do_msync((unsigned long)a1, (size_t)a2, (int)a3)) \
    X(SYS_SENDFILE, sendfile, do_sendfile()) \
    X(SYS_LCHOWN, lchown, do_fchownat(AT_FDCWD, (const char *)a1, (uint32_t)a2, (uint32_t)a3, AT_SYMLINK_NOFOLLOW)) \
    X(SYS_SCHED_GET_PRIORITY_MAX, sched_get_priority_max, do_sched_get_priority_max((int)a1)) \
    X(SYS_SCHED_GET_PRIORITY_MIN, sched_get_priority_min, do_sched_get_priority_min((int)a1)) \
    X(SYS_SETRLIMIT, setrlimit, do_setrlimit()) \
    X(SYS_FADVISE64, fadvise64, do_fadvise64((int)a1, (long)a2, (long)a3, (int)a4)) \
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
    X(SYS_ACCEPT, accept, do_accept4((int)a1, (void *)a2, (int *)a3, 0)) \
    X(SYS_ACCEPT4, accept4, do_accept4((int)a1, (void *)a2, (int *)a3, (int)a4)) \
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
    X(SYS_FCHMODAT, fchmodat, do_fchmodat((int)a1, (const char *)a2, (uint32_t)a3, 0)) \
    X(SYS_UTIMENSAT, utimensat, do_utimensat((int)a1, (const char *)a2, (const void *)a3, (int)a4)) \
    X(SYS_FALLOCATE, fallocate, do_fallocate((int)a1, (int)a2, (int64_t)a3, (int64_t)a4)) \
    X(SYS_MKNODAT, mknodat, do_mknodat((int)a1, (const char *)a2, (uint32_t)a3, (uint64_t)a4)) \
    X(SYS_FSYNC, fsync, do_fsync((int)a1)) \
    X(SYS_FDATASYNC, fdatasync, do_fdatasync((int)a1)) \
    X(SYS_GETDENTS, getdents, do_getdents((int)a1, (void *)a2, (size_t)a3)) \
    X(SYS_FCHDIR, fchdir, do_fchdir((int)a1)) \
    X(SYS_CREAT, creat, do_creat((const char *)a1, (int)a2)) \
    X(SYS_CHOWN, chown, do_fchownat(AT_FDCWD, (const char *)a1, (uint32_t)a2, (uint32_t)a3, 0)) \
    X(SYS_FCHOWNAT, fchownat, do_fchownat((int)a1, (const char *)a2, (uint32_t)a3, (uint32_t)a4, (int)a5)) \
    X(SYS_RENAMEAT, renameat, do_renameat((int)a1, (const char *)a2, (int)a3, (const char *)a4)) \
    X(SYS_UTIME, utime, do_utime((const char *)a1, (const void *)a2)) \
    X(SYS_SYNC, sync, do_sync()) \
    X(SYS_SYNCFS, syncfs, do_syncfs((int)a1)) \
    X(SYS_UMOUNT2, umount2, do_umount2((const char *)a1, (int)a2)) \
    X(SYS_MEMFD_CREATE, memfd_create, do_memfd_create((const char *)a1, (unsigned int)a2)) \
    X(SYS_COPY_FILE_RANGE, copy_file_range, do_copy_file_range((int)a1, (long *)a2, (int)a3, (long *)a4, (size_t)a5, (unsigned int)a6)) \
    X(SYS_CLOSE_RANGE, close_range, do_close_range((unsigned int)a1, (unsigned int)a2, (unsigned int)a3)) \
    X(SYS_FACCESSAT2, faccessat2, do_faccessat2((int)a1, (const char *)a2, (int)a3, (int)a4)) \
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
    X(SYS_EPOLL_PWAIT, epoll_pwait, do_epoll_pwait((int)a1, (struct epoll_event *)a2, (int)a3, (int)a4, (const uint64_t *)a5, (size_t)a6)) \
    /* select / pselect / ppoll */ \
    X(SYS_SELECT, select, do_pselect6((int)a1, (uint64_t *)a2, a3, a4, a5, SYS_SELECT)) \
    X(SYS_PSELECT6, pselect6, do_pselect6((int)a1, (uint64_t *)a2, a3, a4, a5, SYS_PSELECT6)) \
    X(SYS_PPOLL, ppoll, do_ppoll(a1, a2, a3)) \
    /* vectored I/O with offset (x86_64: offset split into low/high 32-bit in a4/a5) */ \
    X(SYS_PREADV, preadv, do_preadv((int)a1, (const struct iovec *)a2, (int)a3, (int64_t)((uint32_t)a4 | ((uint64_t)(uint32_t)a5 << 32)))) \
    X(SYS_PWRITEV, pwritev, do_pwritev((int)a1, (const struct iovec *)a2, (int)a3, (int64_t)((uint32_t)a4 | ((uint64_t)(uint32_t)a5 << 32)))) \
    /* multi-message send/recv */ \
    X(SYS_SENDMMSG, sendmmsg, do_sendmmsg((int)a1, (uint64_t)a2, (int)a3, (int)a4)) \
    X(SYS_RECVMMSG, recvmmsg, do_recvmmsg((int)a1, (uint64_t)a2, (int)a3, (int)a4)) \
    /* ── Batch 2: remaining syscalls (no more "unhandled") ── */ \
    /* Harmless no-ops / delegations */ \
    X(SYS_MINCORE, mincore, do_mincore()) \
    X(SYS_PTRACE, ptrace, do_ptrace()) \
    X(SYS_SYSLOG, syslog, do_syslog_stub()) \
    X(SYS_SCHED_RR_GET_INTERVAL, sched_rr_get_interval, do_sched_rr_get_interval((int)a1, (void *)a2)) \
    X(SYS_VHANGUP, vhangup, do_vhangup()) \
    X(SYS_ADJTIMEX, adjtimex, do_adjtimex((void *)a1)) \
    X(SYS_CHROOT, chroot, do_chroot((const char *)a1)) \
    X(SYS_ACCT, acct, do_acct((const char *)a1)) \
    X(SYS_SETTIMEOFDAY, settimeofday, do_settimeofday()) \
    X(SYS_SETDOMAINNAME, setdomainname, do_setdomainname()) \
    X(SYS_READAHEAD, readahead, do_readahead()) \
    X(SYS_RESTART_SYSCALL, restart_syscall, do_restart_syscall()) \
    X(SYS_CLOCK_SETTIME, clock_settime, do_clock_settime((int)a1, (const void *)a2)) \
    X(SYS_CLOCK_ADJTIME, clock_adjtime, do_clock_adjtime((int)a1, (void *)a2)) \
    X(SYS_UNSHARE, unshare, do_unshare((unsigned long)a1)) \
    X(SYS_UTIMES, utimes, do_utimes((const char *)a1, (const void *)a2)) \
    X(SYS_FUTIMESAT, futimesat, do_futimesat((int)a1, (const char *)a2, (const void *)a3)) \
    X(SYS_SIGNALFD, signalfd, do_signalfd((int)a1, (const uint64_t *)a2)) \
    X(SYS_EVENTFD, eventfd, do_eventfd((unsigned int)a1)) \
    X(SYS_TIMERFD_GETTIME, timerfd_gettime, do_timerfd_gettime((int)a1, (void *)a2)) \
    X(SYS_RT_TGSIGQUEUEINFO, rt_tgsigqueueinfo, do_rt_tgsigqueueinfo((int)a1, (int)a2, (int)a3, (void *)a4)) \
    X(SYS_EPOLL_CREATE, epoll_create, do_epoll_create((int)a1)) \
    X(SYS_INOTIFY_INIT, inotify_init, do_inotify_init()) \
    X(SYS_PREADV2, preadv2, do_preadv((int)a1, (const struct iovec *)a2, (int)a3, (int64_t)((uint32_t)a4 | ((uint64_t)(uint32_t)a5 << 32)))) \
    X(SYS_PWRITEV2, pwritev2, do_pwritev((int)a1, (const struct iovec *)a2, (int)a3, (int64_t)((uint32_t)a4 | ((uint64_t)(uint32_t)a5 << 32)))) \
    X(SYS_OPENAT2, openat2, do_openat2((int)a1, (const char *)a2, (void *)a3, (size_t)a4)) \
    X(SYS_EPOLL_PWAIT2, epoll_pwait2, do_epoll_pwait2((int)a1, (struct epoll_event *)a2, (int)a3, (void *)a4, (const uint64_t *)a5, (size_t)a6)) \
    X(SYS_MKNOD, mknod, do_mknod((const char *)a1, (uint32_t)a2, (uint64_t)a3)) \
    X(SYS_FCHMODAT2, fchmodat2, do_fchmodat2((int)a1, (const char *)a2, (uint32_t)a3, (int)a4)) \
    /* SysV IPC */ \
    X(SYS_SHMGET, shmget, do_shmget((int32_t)a1, (size_t)a2, (int)a3)) \
    X(SYS_SHMAT, shmat, do_shmat((int)a1, (const void *)a2, (int)a3)) \
    X(SYS_SHMCTL, shmctl, do_shmctl((int)a1, (int)a2, (void *)a3)) \
    X(SYS_SHMDT, shmdt, do_shmdt((const void *)a1)) \
    X(SYS_SEMGET, semget, do_semget((int32_t)a1, (int)a2, (int)a3)) \
    X(SYS_SEMOP, semop, do_semop((int)a1, (const void *)a2, (size_t)a3)) \
    X(SYS_SEMCTL, semctl, do_semctl((int)a1, (int)a2, (int)a3, (long)a4)) \
    X(SYS_MSGGET, msgget, do_msgget((int32_t)a1, (int)a2)) \
    X(SYS_MSGSND, msgsnd, do_msgsnd((int)a1, (const void *)a2, (size_t)a3, (int)a4)) \
    X(SYS_MSGRCV, msgrcv, do_msgrcv((int)a1, (void *)a2, (size_t)a3, (long)a4, (int)a5)) \
    X(SYS_MSGCTL, msgctl, do_msgctl((int)a1, (int)a2, (void *)a3)) \
    X(SYS_SEMTIMEDOP, semtimedop, do_semop((int)a1, (const void *)a2, (size_t)a3)) \
    /* Return -ENOSYS: kernel modules */ \
    X(SYS_CREATE_MODULE, create_module, -ENOSYS) \
    X(SYS_INIT_MODULE, init_module, -ENOSYS) \
    X(SYS_DELETE_MODULE, delete_module, -ENOSYS) \
    X(SYS_GET_KERNEL_SYMS, get_kernel_syms, -ENOSYS) \
    X(SYS_QUERY_MODULE, query_module, -ENOSYS) \
    X(SYS_FINIT_MODULE, finit_module, -ENOSYS) \
    /* Return -ENOSYS: security/sandboxing */ \
    X(SYS_SECCOMP, seccomp, -ENOSYS) \
    X(SYS_LANDLOCK_CREATE_RULESET, landlock_create_ruleset, -ENOSYS) \
    X(SYS_LANDLOCK_ADD_RULE, landlock_add_rule, -ENOSYS) \
    X(SYS_LANDLOCK_RESTRICT_SELF, landlock_restrict_self, -ENOSYS) \
    X(SYS_LSM_GET_SELF_ATTR, lsm_get_self_attr, -ENOSYS) \
    X(SYS_LSM_SET_SELF_ATTR, lsm_set_self_attr, -ENOSYS) \
    X(SYS_LSM_LIST_MODULES, lsm_list_modules, -ENOSYS) \
    X(SYS_BPF, bpf, -ENOSYS) \
    /* Return -ENOSYS: NUMA */ \
    X(SYS_MBIND, mbind, -ENOSYS) \
    X(SYS_SET_MEMPOLICY, set_mempolicy, -ENOSYS) \
    X(SYS_GET_MEMPOLICY, get_mempolicy, -ENOSYS) \
    X(SYS_MIGRATE_PAGES, migrate_pages, -ENOSYS) \
    X(SYS_MOVE_PAGES, move_pages, -ENOSYS) \
    X(SYS_SET_MEMPOLICY_HOME_NODE, set_mempolicy_home_node, -ENOSYS) \
    /* Return -ENOSYS: namespaces */ \
    X(SYS_SETNS, setns, do_setns((int)a1, (int)a2)) \
    /* Return -ENOSYS: io_uring */ \
    X(SYS_IO_URING_SETUP, io_uring_setup, -ENOSYS) \
    X(SYS_IO_URING_ENTER, io_uring_enter, -ENOSYS) \
    X(SYS_IO_URING_REGISTER, io_uring_register, -ENOSYS) \
    /* Return -ENOSYS: old AIO */ \
    X(SYS_IO_SETUP, io_setup, -ENOSYS) \
    X(SYS_IO_DESTROY, io_destroy, -ENOSYS) \
    X(SYS_IO_GETEVENTS, io_getevents, -ENOSYS) \
    X(SYS_IO_SUBMIT, io_submit, -ENOSYS) \
    X(SYS_IO_CANCEL, io_cancel, -ENOSYS) \
    X(SYS_IO_PGETEVENTS, io_pgetevents, -ENOSYS) \
    /* Return -ENOSYS: obsolete/misc */ \
    X(SYS_USELIB, uselib, -ENOSYS) \
    X(SYS_NFSSERVCTL, nfsservctl, -ENOSYS) \
    X(SYS_GETPMSG, getpmsg, -ENOSYS) \
    X(SYS_PUTPMSG, putpmsg, -ENOSYS) \
    X(SYS_AFS_SYSCALL, afs_syscall, -ENOSYS) \
    X(SYS_TUXCALL, tuxcall, -ENOSYS) \
    X(SYS_SECURITY, security, -ENOSYS) \
    X(SYS_VSERVER, vserver, -ENOSYS) \
    X(SYS_LOOKUP_DCOOKIE, lookup_dcookie, -ENOSYS) \
    X(SYS_REMAP_FILE_PAGES, remap_file_pages, -ENOSYS) \
    X(SYS_ADD_KEY, add_key, -ENOSYS) \
    X(SYS_REQUEST_KEY, request_key, -ENOSYS) \
    X(SYS_KEYCTL, keyctl, -ENOSYS) \
    X(SYS_IOPRIO_SET, ioprio_set, -ENOSYS) \
    X(SYS_IOPRIO_GET, ioprio_get, -ENOSYS) \
    X(SYS_KEXEC_LOAD, kexec_load, -ENOSYS) \
    X(SYS_KEXEC_FILE_LOAD, kexec_file_load, -ENOSYS) \
    X(SYS_PERF_EVENT_OPEN, perf_event_open, -ENOSYS) \
    X(SYS_PROCESS_VM_READV, process_vm_readv, -ENOSYS) \
    X(SYS_PROCESS_VM_WRITEV, process_vm_writev, -ENOSYS) \
    X(SYS_PROCESS_MADVISE, process_madvise, -ENOSYS) \
    X(SYS_PROCESS_MRELEASE, process_mrelease, -ENOSYS) \
    /* Return -ENOSYS: new mount API */ \
    X(SYS_OPEN_TREE, open_tree, -ENOSYS) \
    X(SYS_MOVE_MOUNT, move_mount, -ENOSYS) \
    X(SYS_FSOPEN, fsopen, -ENOSYS) \
    X(SYS_FSCONFIG, fsconfig, -ENOSYS) \
    X(SYS_FSMOUNT, fsmount, -ENOSYS) \
    X(SYS_FSPICK, fspick, -ENOSYS) \
    X(SYS_MOUNT_SETATTR, mount_setattr, -ENOSYS) \
    X(SYS_OPEN_TREE_ATTR, open_tree_attr, -ENOSYS) \
    /* Return -ENOSYS: pidfd */ \
    X(SYS_PIDFD_OPEN, pidfd_open, -ENOSYS) \
    X(SYS_PIDFD_SEND_SIGNAL, pidfd_send_signal, -ENOSYS) \
    X(SYS_PIDFD_GETFD, pidfd_getfd, -ENOSYS) \
    /* Return -ENOSYS: misc modern */ \
    X(SYS_KCMP, kcmp, -ENOSYS) \
    X(SYS_USERFAULTFD, userfaultfd, -ENOSYS) \
    X(SYS_MEMBARRIER, membarrier, -ENOSYS) \
    X(SYS_MLOCK2, mlock2, -ENOSYS) \
    X(SYS_PKEY_MPROTECT, pkey_mprotect, -ENOSYS) \
    X(SYS_PKEY_ALLOC, pkey_alloc, -ENOSYS) \
    X(SYS_PKEY_FREE, pkey_free, -ENOSYS) \
    X(SYS_MAP_SHADOW_STACK, map_shadow_stack, -ENOSYS) \
    X(SYS_FUTEX_WAITV, futex_waitv, -ENOSYS) \
    X(SYS_FUTEX_WAKE, futex_wake, -ENOSYS) \
    X(SYS_FUTEX_WAIT, futex_wait, -ENOSYS) \
    X(SYS_FUTEX_REQUEUE, futex_requeue, -ENOSYS) \
    X(SYS_STATMOUNT, statmount, -ENOSYS) \
    X(SYS_LISTMOUNT, listmount, -ENOSYS) \
    X(SYS_MSEAL, mseal, -ENOSYS) \
    X(SYS_SETXATTRAT, setxattrat, -ENOSYS) \
    X(SYS_GETXATTRAT, getxattrat, -ENOSYS) \
    X(SYS_LISTXATTRAT, listxattrat, -ENOSYS) \
    X(SYS_REMOVEXATTRAT, removexattrat, -ENOSYS) \
    X(SYS_FILE_GETATTR, file_getattr, -ENOSYS) \
    X(SYS_FILE_SETATTR, file_setattr, -ENOSYS) \
    X(SYS_LISTNS, listns, -ENOSYS) \
    X(SYS_RSEQ_SLICE_YIELD, rseq_slice_yield, -ENOSYS) \
    X(SYS_CACHESTAT, cachestat, -ENOSYS) \
    X(SYS_MEMFD_SECRET, memfd_secret, -ENOSYS) \
    X(SYS_URETPROBE, uretprobe, -ENOSYS) \
    X(SYS_UPROBE, uprobe, -ENOSYS) \
    /* Return -ENOSYS: remaining obsolete */ \
    X(SYS_MODIFY_LDT, modify_ldt, -ENOSYS) \
    X(SYS_PIVOT_ROOT, pivot_root, -ENOSYS) \
    X(SYS__SYSCTL, _sysctl, -ENOSYS) \
    X(SYS_SYSFS, sysfs, -ENOSYS) \
    X(SYS_USTAT, ustat, -ENOSYS) \
    X(SYS_SWAPON, swapon, -ENOSYS) \
    X(SYS_SWAPOFF, swapoff, -ENOSYS) \
    X(SYS_QUOTACTL, quotactl, -ENOSYS) \
    X(SYS_SET_THREAD_AREA, set_thread_area, -ENOSYS) \
    X(SYS_GET_THREAD_AREA, get_thread_area, -ENOSYS) \
    X(SYS_EPOLL_CTL_OLD, epoll_ctl_old, -ENOSYS) \
    X(SYS_EPOLL_WAIT_OLD, epoll_wait_old, -ENOSYS) \
    X(SYS_TIMER_CREATE, timer_create, -ENOSYS) \
    X(SYS_TIMER_SETTIME, timer_settime, -ENOSYS) \
    X(SYS_TIMER_GETTIME, timer_gettime, -ENOSYS) \
    X(SYS_TIMER_GETOVERRUN, timer_getoverrun, -ENOSYS) \
    X(SYS_TIMER_DELETE, timer_delete, -ENOSYS) \
    X(SYS_MQ_OPEN, mq_open, -ENOSYS) \
    X(SYS_MQ_UNLINK, mq_unlink, -ENOSYS) \
    X(SYS_MQ_TIMEDSEND, mq_timedsend, -ENOSYS) \
    X(SYS_MQ_TIMEDRECEIVE, mq_timedreceive, -ENOSYS) \
    X(SYS_MQ_NOTIFY, mq_notify, -ENOSYS) \
    X(SYS_MQ_GETSETATTR, mq_getsetattr, -ENOSYS) \
    X(SYS_GET_ROBUST_LIST, get_robust_list, do_get_robust_list((int)a1, (void **)a2, (size_t *)a3)) \
    X(SYS_SPLICE, splice, -ENOSYS) \
    X(SYS_TEE, tee, -ENOSYS) \
    X(SYS_SYNC_FILE_RANGE, sync_file_range, -ENOSYS) \
    X(SYS_VMSPLICE, vmsplice, -ENOSYS) \
    X(SYS_FANOTIFY_INIT, fanotify_init, -ENOSYS) \
    X(SYS_FANOTIFY_MARK, fanotify_mark, -ENOSYS) \
    X(SYS_NAME_TO_HANDLE_AT, name_to_handle_at, -ENOSYS) \
    X(SYS_OPEN_BY_HANDLE_AT, open_by_handle_at, -ENOSYS) \
    X(SYS_SCHED_SETATTR, sched_setattr, -ENOSYS) \
    X(SYS_SCHED_GETATTR, sched_getattr, -ENOSYS) \
    X(SYS_EXECVEAT, execveat, do_execveat((int)a1, (const char *)a2, (char *const *)a3, (char *const *)a4, (int)a5)) \
    X(SYS_QUOTACTL_FD, quotactl_fd, -ENOSYS) \
    /* Return -EPERM: privileged ops */ \
    X(SYS_IOPL, iopl, -EPERM) \
    X(SYS_IOPERM, ioperm, -EPERM) \
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
