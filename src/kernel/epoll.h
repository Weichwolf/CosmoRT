/* CosmoRT epoll/eventfd/timerfd/signalfd/inotify — event multiplexing for libuv/Node.js */
#ifndef EPOLL_H
#define EPOLL_H

#include <stdint.h>
#include <stddef.h>
#include "spinlock.h"

/* k_timespec — shared with syscall.c (both guard against redefinition) */
#ifndef K_TIMESPEC_DEFINED
#define K_TIMESPEC_DEFINED
struct k_timespec { long tv_sec; long tv_nsec; };
#endif

/* epoll_event — packed, matches Linux ABI */
struct epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

/* epoll event flags */
#define EPOLLIN      0x001
#define EPOLLOUT     0x004
#define EPOLLERR     0x008
#define EPOLLHUP     0x010
#define EPOLLET      (1U << 31)

/* epoll_ctl ops */
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

/* EFD_NONBLOCK / EFD_CLOEXEC — same bit values as Linux */
#define EFD_CLOEXEC  02000000
#define EFD_NONBLOCK 04000

/* TFD flags */
#define TFD_CLOEXEC  02000000
#define TFD_NONBLOCK 04000

/* itimerspec for timerfd */
struct k_itimerspec {
    struct k_timespec it_interval;
    struct k_timespec it_value;
};

/* ── eventfd_t — exposed so syscall.c can check .counter for readiness ── */

typedef struct {
    uint64_t   counter;
    int        flags;
    spinlock_t lock;
} eventfd_t;

/* ── timerfd_t — exposed so syscall.c can check expiration for readiness ── */

typedef struct {
    uint64_t   expire_ms;     /* absolute expiration in timer_ms() */
    uint64_t   interval_ms;   /* 0 = one-shot */
    uint64_t   expirations;   /* unread expiration count */
    int        armed;
    int        flags;
    spinlock_t lock;
} timerfd_t;

/* Initialise epoll/eventfd/timerfd slab pools */
void epoll_init(void);

/* Syscall implementations */
long do_epoll_create1(int flags);
long do_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
long do_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);

long do_eventfd2(unsigned int initval, int flags);

long do_timerfd_create(int clockid, int flags);
long do_timerfd_settime(int fd, int tfd_flags,
                        const struct k_itimerspec *new_value,
                        struct k_itimerspec *old_value);

long do_signalfd4(int fd, const uint64_t *mask, int flags);

long do_inotify_init1(int flags);
long do_inotify_add_watch(int fd, const char *path, uint32_t mask);
long do_inotify_rm_watch(int fd, int wd);

/* FD cleanup hooks (called from fd_cleanup_entry / do_close) */
void epoll_destroy(void *obj);
void eventfd_destroy(void *obj);
void timerfd_destroy(void *obj);
void inotify_destroy(void *obj);

/* Read/write hooks for do_read/do_write */
long eventfd_read(void *obj, void *buf, long count);
long eventfd_write(void *obj, const void *buf, long count);
long timerfd_read(void *obj, void *buf, long count);
long inotify_read(void *obj, void *buf, long count);
int  inotify_has_events(void *obj);

/* Queue inotify event (called from VFS operations) */
void inotify_event(const char *path, uint32_t mask);

/* inotify event masks */
#define IN_MODIFY      0x00000002
#define IN_MOVED_FROM  0x00000040
#define IN_MOVED_TO    0x00000080
#define IN_CREATE      0x00000100
#define IN_DELETE       0x00000200

/* FD readiness check — implemented in syscall.c (has access to all FD types) */
uint32_t fd_poll_readiness(int fd, uint32_t interest);

#endif
