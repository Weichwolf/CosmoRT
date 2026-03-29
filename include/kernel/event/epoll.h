/* CosmoRT epoll/eventfd/timerfd/signalfd/inotify — event multiplexing for libuv/Node.js */
#ifndef EPOLL_H
#define EPOLL_H

#include <stdint.h>
#include <stddef.h>
#include "spinlock.h"

/* k_timespec, k_itimerspec, epoll_event, EPOLL_*, EFD_*, TFD_*, IN_*
 * — all from linux.h */
#define __KERNEL__
#include "linux/abi.h"

/* ── eventfd_t — exposed so syscall.c can check .counter for readiness ── */

typedef struct {
    uint64_t   counter;
    int        flags;
    int        refcount;
    spinlock_t lock;
} eventfd_t;

/* ── timerfd_t — exposed so syscall.c can check expiration for readiness ── */

typedef struct {
    uint64_t   expire_ms;     /* absolute expiration in timer_ms() */
    uint64_t   interval_ms;   /* 0 = one-shot */
    uint64_t   expirations;   /* unread expiration count */
    int        armed;
    int        flags;
    int        refcount;
    spinlock_t lock;
} timerfd_t;

/* Initialise epoll/eventfd/timerfd slab pools */
void epoll_init(void);

/* Sub-system init (called from epoll_init) */
void eventfd_init(void);
void timerfd_init(void);
void inotify_init_slab(void);

/* Check if any timerfd has expired — used by epoll_check_timeouts */
int timerfd_any_expired(void);

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
void epoll_incref(void *obj);
void epoll_destroy(void *obj);
void eventfd_destroy(void *obj);
void timerfd_destroy(void *obj);
void inotify_incref(void *obj);
void inotify_destroy(void *obj);

/* Read/write hooks for do_read/do_write */
long eventfd_read(void *obj, void *buf, long count);
long eventfd_write(void *obj, const void *buf, long count);
long timerfd_read(void *obj, void *buf, long count);
long inotify_read(void *obj, void *buf, long count);
int  inotify_has_events(void *obj);

/* Queue inotify event (called from VFS operations) */
void inotify_event(const char *path, uint32_t mask);

/* FD readiness check — implemented in syscall.c (has access to all FD types) */
uint32_t fd_poll_readiness(int fd, uint32_t interest);

/* Wake all threads blocked in epoll_wait/poll.
 * Called from eventfd_write, pipe_write, timerfd expire, socket recv.
 * IRQ-safe (uses spin_lock_irq internally). */
void epoll_wake_all(void);

/* Check timed-out epoll sleepers. Called from timer IRQ (sched_preempt). */
void epoll_check_timeouts(void);

#endif
