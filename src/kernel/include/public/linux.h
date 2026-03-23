/* CosmoRT User-Space API — all user-visible constants and struct layouts
 *
 * This is the SINGLE source of truth for:
 *   - Linux x86_64 syscall numbers
 *   - errno values
 *   - mmap/mprotect flags
 *   - open/fcntl flags
 *   - signal numbers and SA_* flags
 *   - clock IDs
 *   - AT_* constants
 *   - struct layouts crossing the kernel/user boundary
 *   - CosmoRT-specific syscall numbers (512+)
 *
 * Kernel headers #include this instead of defining their own copies.
 * Test harness (ktest.h) #includes this instead of duplicating.
 */
#ifndef COSMO_UAPI_H
#define COSMO_UAPI_H

#ifdef __KERNEL__
#include <stdint.h>
#include <stddef.h>
#else
/* Freestanding userspace — define types directly */
typedef unsigned long      uint64_t;
typedef long               int64_t;
typedef unsigned int       uint32_t;
typedef int                int32_t;
typedef unsigned short     uint16_t;
typedef short              int16_t;
typedef unsigned char      uint8_t;
typedef unsigned long      size_t;
typedef long               ssize_t;
#define NULL ((void *)0)
#endif

/* ══════════════════════════════════════════════════════════════════
 * Syscall numbers — Linux x86_64 ABI
 * ══════════════════════════════════════════════════════════════════ */

/* Core */
#define SYS_READ            0
#define SYS_WRITE           1
#define SYS_OPEN            2
#define SYS_CLOSE           3
#define SYS_STAT            4
#define SYS_FSTAT           5
#define SYS_LSTAT           6
#define SYS_POLL            7
#define SYS_LSEEK           8
#define SYS_MMAP            9
#define SYS_MPROTECT        10
#define SYS_MUNMAP          11
#define SYS_BRK             12
#define SYS_RT_SIGACTION    13
#define SYS_RT_SIGPROCMASK  14
#define SYS_RT_SIGRETURN    15
#define SYS_IOCTL           16
#define SYS_READV           19
#define SYS_WRITEV          20
#define SYS_ACCESS          21
#define SYS_PIPE            22
#define SYS_SCHED_YIELD     24
#define SYS_MREMAP          25
#define SYS_MADVISE         28
#define SYS_DUP             32
#define SYS_DUP2            33
#define SYS_NANOSLEEP       35
#define SYS_GETPID          39

/* Sockets */
#define SYS_SOCKET          41
#define SYS_CONNECT         42
#define SYS_ACCEPT          43
#define SYS_SENDTO          44
#define SYS_RECVFROM        45
#define SYS_SENDMSG         46
#define SYS_RECVMSG         47
#define SYS_SHUTDOWN        48
#define SYS_BIND            49
#define SYS_LISTEN          50
#define SYS_GETSOCKNAME     51
#define SYS_GETPEERNAME     52
#define SYS_SOCKETPAIR      53
#define SYS_SETSOCKOPT      54
#define SYS_GETSOCKOPT      55

/* Process */
#define SYS_CLONE           56
#define SYS_FORK            57
#define SYS_VFORK           58
#define SYS_EXECVE          59
#define SYS_EXIT            60
#define SYS_WAIT4           61
#define SYS_KILL            62
#define SYS_UNAME           63
#define SYS_FCNTL           72
#define SYS_TRUNCATE        76
#define SYS_FTRUNCATE       77
#define SYS_GETCWD          79
#define SYS_CHDIR           80
#define SYS_RENAME          82
#define SYS_MKDIR           83
#define SYS_RMDIR           84
#define SYS_LINK            86
#define SYS_UNLINK          87
#define SYS_SYMLINK         88
#define SYS_READLINK        89
#define SYS_FCHMOD          91
#define SYS_FCHOWN          93
#define SYS_GETTIMEOFDAY    96
#define SYS_GETRLIMIT       97
#define SYS_GETRUSAGE       98
#define SYS_SYSINFO         99
#define SYS_TIMES           100
#define SYS_GETUID          102
#define SYS_GETGID          104
#define SYS_GETEUID         107
#define SYS_GETEGID         108
#define SYS_SETPGID         109
#define SYS_GETPPID         110
#define SYS_GETPGRP         111
#define SYS_SETSID          112
#define SYS_GETPGID         121
#define SYS_GETSID          124
#define SYS_CAPGET          125
#define SYS_CAPSET          126
#define SYS_RT_SIGSUSPEND   130
#define SYS_SIGALTSTACK     131
#define SYS_STATFS          137
#define SYS_FSTATFS         138
#define SYS_SCHED_SETPARAM    142
#define SYS_SCHED_GETPARAM    143
#define SYS_SCHED_SETSCHEDULER 144
#define SYS_SCHED_GETSCHEDULER 145
#define SYS_MLOCK             149
#define SYS_MUNLOCK           150
#define SYS_MLOCKALL          151
#define SYS_MUNLOCKALL        152
#define SYS_PRCTL             157
#define SYS_ARCH_PRCTL      158
#define SYS_MOUNT           165
#define SYS_SETHOSTNAME     170
#define SYS_GETTID          186
#define SYS_TIME            201
#define SYS_FUTEX           202
#define SYS_SCHED_SETAFFINITY 203
#define SYS_SCHED_GETAFFINITY 204
#define SYS_GETDENTS64      217
#define SYS_SET_TID_ADDRESS  218
#define SYS_CLOCK_GETTIME   228
#define SYS_CLOCK_GETRES    229
#define SYS_CLOCK_NANOSLEEP 230
#define SYS_EXIT_GROUP      231
#define SYS_EPOLL_WAIT      232
#define SYS_EPOLL_CTL       233
#define SYS_TGKILL          234
#define SYS_INOTIFY_ADD_WATCH 254
#define SYS_INOTIFY_RM_WATCH  255
#define SYS_OPENAT           257
#define SYS_MKDIRAT          258
#define SYS_MKNODAT          259
#define SYS_FSTATAT          262
#define SYS_UNLINKAT         263
#define SYS_READLINKAT       267
#define SYS_FCHMODAT         268
#define SYS_FACCESSAT        269
#define SYS_SET_ROBUST_LIST 273
#define SYS_UTIMENSAT        280
#define SYS_EPOLL_PWAIT     281
#define SYS_TIMERFD_CREATE  283
#define SYS_FALLOCATE        285
#define SYS_TIMERFD_SETTIME 286
#define SYS_ACCEPT4          288
#define SYS_SIGNALFD4       289
#define SYS_EVENTFD2        290
#define SYS_EPOLL_CREATE1   291
#define SYS_DUP3             292
#define SYS_PIPE2            293
#define SYS_INOTIFY_INIT1  294
#define SYS_PRLIMIT64       302
#define SYS_RENAMEAT2        316
#define SYS_GETRANDOM       318
#define SYS_RSEQ            334
#define SYS_CLONE3           435

/* CosmoRT hardware primitives (non-POSIX, for userspace drivers) */
#define SYS_COSMO_MMIO_MAP       512
#define SYS_COSMO_DMA_ALLOC      513
#define SYS_COSMO_DMA_FREE       514
#define SYS_COSMO_IRQ_REGISTER   515
#define SYS_COSMO_PCI_READ       516
#define SYS_COSMO_PCI_WRITE      517
#define SYS_COSMO_FW_LOAD        518
#define SYS_COSMO_NIC_ATTACH     519
#define SYS_COSMO_KEXEC          520

/* ══════════════════════════════════════════════════════════════════
 * Error numbers (Linux-compatible)
 * ══════════════════════════════════════════════════════════════════ */

#define EPERM           1
#define ENOENT          2
#define ESRCH           3
#define EINTR           4
#define EIO             5
#define ENOEXEC         8
#define EBADF           9
#define ECHILD          10
#define EAGAIN          11
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define EEXIST          17
#define ENOTDIR         20
#define EISDIR          21
#define EINVAL          22
#define EMFILE          24
#define ENOTTY          25
#define EPIPE           32
#define ERANGE          34
#define ENAMETOOLONG    36
#define ENOSYS          38
#define ENOTEMPTY       39
#define ELOOP           40
#define ENOTSOCK        88
#define EDESTADDRREQ    89
#define EMSGSIZE        90
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP      95
#define EAFNOSUPPORT    97
#define EADDRINUSE      98
#define ENETUNREACH     101
#define ECONNRESET      104
#define EISCONN         106
#define ENOTCONN        107
#define ETIMEDOUT       110
#define ECONNREFUSED    111

/* ══════════════════════════════════════════════════════════════════
 * Memory mapping flags (Linux-compatible)
 * ══════════════════════════════════════════════════════════════════ */

#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

#define MAP_PRIVATE          0x02
#define MAP_FIXED            0x10
#define MAP_ANONYMOUS        0x20
#define MAP_FIXED_NOREPLACE  0x100000

/* mremap flags */
#define MREMAP_MAYMOVE  1
#define MREMAP_FIXED    2

/* mlockall flags */
#define MCL_CURRENT     1
#define MCL_FUTURE      2

/* ══════════════════════════════════════════════════════════════════
 * Open / fcntl flags (Linux-compatible)
 * ══════════════════════════════════════════════════════════════════ */

#define O_RDONLY        0x0000
#define O_WRONLY        0x0001
#define O_RDWR          0x0002
#define O_CREAT         0x0040
#define O_TRUNC         0x0200
#define O_APPEND        0x0400
#define O_NONBLOCK      0x0800
#define O_DIRECTORY     0x10000
#define O_CLOEXEC       0x80000

/* fcntl commands */
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC      1

/* Seek */
#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2

/* AT constants */
#define AT_FDCWD              (-100)
#define AT_SYMLINK_NOFOLLOW   0x100
#define AT_REMOVEDIR          0x200

/* ══════════════════════════════════════════════════════════════════
 * arch_prctl codes
 * ══════════════════════════════════════════════════════════════════ */

#define ARCH_SET_GS     0x1001
#define ARCH_SET_FS     0x1002
#define ARCH_GET_FS     0x1003
#define ARCH_GET_GS     0x1004

/* ══════════════════════════════════════════════════════════════════
 * Signals (Linux-compatible)
 * ══════════════════════════════════════════════════════════════════ */

#define SIGINT          2
#define SIGQUIT         3
#define SIGKILL         9
#define SIGUSR1         10
#define SIGSEGV         11
#define SIGUSR2         12
#define SIGPIPE         13
#define SIGTERM         15
#define SIGCHLD         17
#define SIGSTOP         19

#define SA_SIGINFO      0x00000004
#define SA_RESTART      0x10000000
#define SA_RESTORER     0x04000000

#define SIG_DFL         ((void *)0)
#define SIG_IGN         ((void *)1)

/* ══════════════════════════════════════════════════════════════════
 * Clock IDs (POSIX / Linux)
 * ══════════════════════════════════════════════════════════════════ */

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define TIMER_ABSTIME   1

/* ══════════════════════════════════════════════════════════════════
 * clone flags
 * ══════════════════════════════════════════════════════════════════ */

#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SYSVSEM        0x00040000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

/* ══════════════════════════════════════════════════════════════════
 * Socket constants
 * ══════════════════════════════════════════════════════════════════ */

#define AF_UNIX         1
#define AF_INET         2
#define AF_INET6        10
#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_KEEPALIVE    9
#define IPPROTO_TCP     6
#define TCP_NODELAY     1

#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

/* ══════════════════════════════════════════════════════════════════
 * epoll / eventfd / timerfd / inotify constants
 * ══════════════════════════════════════════════════════════════════ */

#define EPOLLIN         0x001
#define EPOLLOUT        0x004
#define EPOLLERR        0x008
#define EPOLLHUP        0x010
#define EPOLLET         (1U << 31)

#define EPOLL_CTL_ADD   1
#define EPOLL_CTL_DEL   2
#define EPOLL_CTL_MOD   3

#define EFD_CLOEXEC     02000000
#define EFD_NONBLOCK    04000
#define TFD_CLOEXEC     02000000
#define TFD_NONBLOCK    04000

#define IN_MODIFY       0x00000002
#define IN_MOVED_FROM   0x00000040
#define IN_MOVED_TO     0x00000080
#define IN_CREATE       0x00000100
#define IN_DELETE       0x00000200

/* ══════════════════════════════════════════════════════════════════
 * Stat struct — Linux x86_64 layout
 * ══════════════════════════════════════════════════════════════════ */

/* st_mode bits */
#define S_IFMT          0170000
#define S_IFCHR         0020000
#define S_IFIFO         0010000
#define S_IFREG         0100000
#define S_IFDIR         0040000
#define S_IFLNK         0120000
#define S_IFSOCK        0140000

struct k_stat {
    uint64_t st_dev, st_ino;
    uint64_t st_nlink;
    uint32_t st_mode, st_uid, st_gid, __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize, st_blocks;
    int64_t  st_atime_sec, st_atime_nsec;
    int64_t  st_mtime_sec, st_mtime_nsec;
    int64_t  st_ctime_sec, st_ctime_nsec;
    int64_t  __unused[3];
};

/* ══════════════════════════════════════════════════════════════════
 * Timespec — shared kernel/user boundary struct
 * ══════════════════════════════════════════════════════════════════ */

#ifndef K_TIMESPEC_DEFINED
#define K_TIMESPEC_DEFINED
struct k_timespec { long tv_sec; long tv_nsec; };
#endif

struct k_timeval { long tv_sec; long tv_usec; };

struct k_itimerspec {
    struct k_timespec it_interval;
    struct k_timespec it_value;
};

/* ══════════════════════════════════════════════════════════════════
 * Signal action — Linux-compatible layout for rt_sigaction
 * ══════════════════════════════════════════════════════════════════ */

struct k_sigaction {
    void    *sa_handler;   /* SIG_DFL=0, SIG_IGN=1, or handler address */
    uint64_t sa_flags;
    void    *sa_restorer;
    uint64_t sa_mask;
};

/* ══════════════════════════════════════════════════════════════════
 * epoll_event — packed, matches Linux ABI
 * ══════════════════════════════════════════════════════════════════ */

struct epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

#endif /* COSMO_UAPI_H */
