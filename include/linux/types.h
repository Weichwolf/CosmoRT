/* Linux x86_64 ABI — base types and struct layouts */
#ifndef COSMO_LINUX_TYPES_H
#define COSMO_LINUX_TYPES_H

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

/* ── Stat struct — Linux x86_64 layout ── */

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

/* ── Timespec — shared kernel/user boundary struct ── */

#ifndef K_TIMESPEC_DEFINED
#define K_TIMESPEC_DEFINED
struct k_timespec { long tv_sec; long tv_nsec; };
#endif

struct k_timeval { long tv_sec; long tv_usec; };

struct k_itimerspec {
    struct k_timespec it_interval;
    struct k_timespec it_value;
};

/* ── Signal action — Linux-compatible layout for rt_sigaction ── */

struct k_sigaction {
    void    *sa_handler;   /* SIG_DFL=0, SIG_IGN=1, or handler address */
    uint64_t sa_flags;
    void    *sa_restorer;
    uint64_t sa_mask;
};

/* ── epoll_event — packed, matches Linux ABI ── */

struct epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

#endif /* COSMO_LINUX_TYPES_H */
