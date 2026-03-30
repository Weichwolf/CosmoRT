/* CosmoRT epoll/eventfd/timerfd/signalfd/inotify — event multiplexing for libuv/Node.js */
#ifndef EPOLL_H
#define EPOLL_H

#include <stdint.h>
#include <stddef.h>
#include "core/mutex.h"

#define __KERNEL__
#include "linux/abi.h"

typedef struct {
    uint64_t   counter;
    int        flags;
    int        refcount;
    mutex_t lock;
} eventfd_t;

typedef struct {
    uint64_t   expire_ms;
    uint64_t   interval_ms;
    uint64_t   expirations;
    uint64_t   expire_tsc;
    uint64_t   interval_tsc;
    int        armed;
    int        flags;
    int        refcount;
    mutex_t lock;
} timerfd_t;

void epoll_init(void);

void eventfd_init(void);
void timerfd_init(void);
void inotify_init_slab(void);

int timerfd_any_expired(void);

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

void epoll_incref(void *obj);
void epoll_destroy(void *obj);
void eventfd_destroy(void *obj);
void timerfd_destroy(void *obj);
void inotify_incref(void *obj);
void inotify_destroy(void *obj);

long eventfd_read(void *obj, void *buf, long count);
long eventfd_write(void *obj, const void *buf, long count);
long timerfd_read(void *obj, void *buf, long count);
long inotify_read(void *obj, void *buf, long count);
int  inotify_has_events(void *obj);

void inotify_event(const char *path, uint32_t mask);

uint32_t fd_poll_readiness(int fd, uint32_t interest);

#endif
