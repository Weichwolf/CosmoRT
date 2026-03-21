/* CosmoRT POSIX Syscall Layer — Linux x86_64 syscall numbers */
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/* Linux x86_64 syscall numbers (Phase 2a: minimal for Hello World) */
#define SYS_READ            0
#define SYS_WRITE           1
#define SYS_OPEN            2
#define SYS_CLOSE           3
#define SYS_STAT            4
#define SYS_FSTAT           5
#define SYS_MMAP            9
#define SYS_MPROTECT        10
#define SYS_MUNMAP          11
#define SYS_BRK             12
#define SYS_RT_SIGACTION    13
#define SYS_RT_SIGPROCMASK  14
#define SYS_IOCTL           16
#define SYS_WRITEV          20
#define SYS_ACCESS          21
#define SYS_GETPID          39
#define SYS_CLONE           56
#define SYS_FORK            57
#define SYS_EXECVE          59
#define SYS_EXIT            60
#define SYS_UNAME           63
#define SYS_FCNTL           72
#define SYS_GETUID          102
#define SYS_GETGID          104
#define SYS_GETEUID         107
#define SYS_GETEGID         108
#define SYS_ARCH_PRCTL      158
#define SYS_GETTID          186
#define SYS_SET_TID_ADDRESS  218
#define SYS_EXIT_GROUP      231
#define SYS_SET_ROBUST_LIST 273
#define SYS_PRLIMIT64       302
#define SYS_GETRANDOM       318
#define SYS_SCHED_SETPARAM    142
#define SYS_SCHED_GETPARAM    143
#define SYS_SCHED_SETSCHEDULER 144
#define SYS_SCHED_GETSCHEDULER 145
#define SYS_MLOCK             149
#define SYS_MUNLOCK           150
#define SYS_MLOCKALL          151
#define SYS_MUNLOCKALL        152
#define SYS_FUTEX             202
#define SYS_SCHED_SETAFFINITY 203
#define SYS_SCHED_GETAFFINITY 204
#define SYS_RSEQ              334

/* Network / sockets */
#define SYS_POLL            7
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

/* sched_yield — already used via SYS_SCHED_YIELD */
#define SYS_SCHED_YIELD       24
#define SYS_DUP2              33
#define SYS_WAIT4             61
#define SYS_KILL              62
#define SYS_GETCWD            79
#define SYS_CHDIR             80
#define SYS_LSEEK             8
#define SYS_PIPE2             293
#define SYS_OPENAT            257
#define SYS_READV             19
#define SYS_RT_SIGRETURN      15
#define SYS_GETPPID           110

/* Timers / clocks */
#define SYS_NANOSLEEP         35
#define SYS_GETTIMEOFDAY      96
#define SYS_CLOCK_GETTIME     228
#define SYS_CLOCK_GETRES      229
#define SYS_CLOCK_NANOSLEEP   230

/* CosmoRT hardware primitives (non-POSIX, for userspace drivers) */
#define SYS_COSMO_MMIO_MAP       512
#define SYS_COSMO_DMA_ALLOC      513
#define SYS_COSMO_DMA_FREE       514
#define SYS_COSMO_IRQ_REGISTER   515
#define SYS_COSMO_PCI_READ       516
#define SYS_COSMO_PCI_WRITE      517
#define SYS_COSMO_FW_LOAD        518
#define SYS_COSMO_NIC_ATTACH     519

/* mmap flags */
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_PRIVATE   0x02

/* arch_prctl codes */
#define ARCH_SET_GS   0x1001
#define ARCH_SET_FS   0x1002
#define ARCH_GET_FS   0x1003
#define ARCH_GET_GS   0x1004

/* mlockall flags */
#define MCL_CURRENT  1
#define MCL_FUTURE   2

/* clock IDs */
#define CLOCK_REALTIME   0
#define CLOCK_MONOTONIC  1

/* TIMER_ABSTIME */
#define TIMER_ABSTIME    1

/* Error numbers */
#define EPERM     1
#define ENOENT    2
#define ESRCH     3
#define EIO       5
#define EBADF     9
#define ECHILD    10
#define EAGAIN    11
#define ENOMEM    12
#define EACCES    13
#define EFAULT    14
#define ENOTDIR   20
#define EISDIR    21
#define EINVAL    22
#define EMFILE    24
#define ENOEXEC   8
#define ENOSYS    38
#define ERANGE    34
#define ETIMEDOUT 110

/* Main syscall dispatcher — called from ASM entry and INT 0x80 */
long sys_handler(long num, long a1, long a2, long a3, long a4, long a5, long a6);

#endif
