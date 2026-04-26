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

#include "core/waitqueue.h"
#include "core/hrtimer.h"

/* ── eventfd_t — exposed so syscall.c can check .counter for readiness ── */

typedef struct {
    uint64_t          counter;
    int               flags;
    int               refcount;
    spinlock_t        lock;
    /* Per-fd waitqueues: Reader parken auf read_wq bis counter > 0, Writer
     * auf write_wq bis Platz vorhanden. Multiple Waiter pro Seite. */
    wait_queue_head_t read_wq;
    wait_queue_head_t write_wq;
} eventfd_t;

/* ── timerfd_t — exposed so syscall.c can check expiration for readiness ── */

typedef struct {
    uint64_t          expire_ms;     /* absolute expiration in timer_ms() */
    uint64_t          interval_ms;   /* 0 = one-shot */
    uint64_t          expirations;   /* unread expiration count */
    int               armed;
    int               flags;
    int               refcount;
    spinlock_t        lock;
    /* Waitqueue for blocking timerfd_read; hrtimer fires wake_up_interruptible
     * on expiry. Multiple readers permitted; close broadcasts via wake_up_all. */
    wait_queue_head_t wq;
    hrtimer_t         timer;
    int               timer_armed;   /* hrtimer_start was called, cancel needed */
} timerfd_t;

/* Initialise epoll/eventfd/timerfd slab pools */
void epoll_init(void);

/* Sub-system init (called from epoll_init) */
void eventfd_init(void);
void timerfd_init(void);
void inotify_init_slab(void);

/* Check if any timerfd has expired — utility used by readiness scans. */
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

/* Read/write hooks for do_read/do_write.
 * nonblock=1 when the caller's fd has O_NONBLOCK; 0 blocks on per-fd wq. */
long eventfd_read(void *obj, void *buf, long count, int nonblock);
long eventfd_write(void *obj, const void *buf, long count, int nonblock);
long timerfd_read(void *obj, void *buf, long count, int nonblock);
long inotify_read(void *obj, void *buf, long count);
int  inotify_has_events(void *obj);

/* Queue inotify event (called from VFS operations) */
void inotify_event(const char *path, uint32_t mask);

/* dnotify (fcntl F_NOTIFY) — directory-watch mit Legacy-SIGIO-Delivery.
 * dnotify_ctl wird aus do_fcntl aufgerufen; dnotify_fire aus VFS-Ops. */
struct process;
long dnotify_ctl(int fd, const char *abspath, uint32_t arg, int sig);
void dnotify_fd_closed(struct process *p, int fd);
void dnotify_proc_exit(struct process *p);
void dnotify_fire(const char *event_path, uint32_t mask);
int  dnotify_queue_pop_fd(struct process *p, int sig);

/* FD readiness check — implemented in syscall.c (has access to all FD types) */
uint32_t fd_poll_readiness(int fd, uint32_t interest);

/* Returns the eventpoll's wq (for nested epoll: ep1 watching ep2). */
wait_queue_head_t *epoll_obj_wq(void *obj);

/* Source-fd waitqueue lookup.
 *
 * For a watched fd and the requested events mask, returns the wait_queue_head_t
 * the watcher (epoll_ctl ADD, poll(2)) should subscribe to. Linux's
 * file_operations.poll feeds wait queues into a poll_table; we simplify by
 * mapping fd type -> wq directly.
 *
 *   FD_EVENTFD     -> read_wq if EPOLLIN, else write_wq.
 *   FD_PIPE        -> read end's read_wq for EPOLLIN, write end's write_wq for EPOLLOUT.
 *   FD_TIMERFD     -> wq.
 *   FD_SOCKET      -> tcp.wait_wq (TCP) or udp_sock_t.recv_wq (UDP).
 *   FD_UNIX_SOCK   -> read_wq for EPOLLIN, write_wq for EPOLLOUT, accept_wq for listening.
 *   FD_PIPE/PTY/INOTIFY/SERIAL/FILE: NULL — readiness re-scan on every wait covers them
 *                                   (no edge-trigger, but level works since epoll_wait
 *                                   re-scans the entry list under ep->lock).
 *
 * Returns NULL when the source has no wq for the requested side. */
wait_queue_head_t *fd_get_poll_wq(int fd, uint32_t events);

#endif
