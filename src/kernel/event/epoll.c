/* CosmoRT epoll — Linux-compatible event multiplexing */

#include "event/epoll.h"
#include "sys/syscall.h"
#include "proc/process.h"
#include "core/percpu.h"
#include "hw/serial.h"
#include "mm/slab.h"
#include "core/timer.h"
#include "event/fd.h"
#include "config.h"
#include "memops.h"
#include "net/net.h"
#include "core/event_queue.h"

#include "uaccess.h"
#include "arch/arch.h"

#define EPOLL_MAX_FDS   64
#define EPOLL_POOL_MAX  16

typedef struct {
    int      fd;
    uint32_t events;
    uint64_t data;
    uint32_t et_armed;
    uint32_t et_last;
} epoll_entry_t;

typedef struct {
    epoll_entry_t entries[EPOLL_MAX_FDS];
    int           count;
    int           refcount;
    mutex_t       lock;
} epoll_t;

static epoll_t epoll_pool[EPOLL_POOL_MAX];
static slab_t  epoll_slab;

void epoll_init(void) {
    slab_init(&epoll_slab, epoll_pool, (int)sizeof(epoll_t), EPOLL_POOL_MAX);
    eventfd_init();
    timerfd_init();
    inotify_init_slab();
    serial_puts("epoll: init\n");
}

long do_epoll_create1(int flags) {
    (void)flags;
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    epoll_t *ep = (epoll_t *)slab_alloc(&epoll_slab);
    if (!ep) return -ENOMEM;

    ep->count = 0;
    ep->refcount = 1;
    ep->lock = (mutex_t)MUTEX_INIT;

    int fd = fd_alloc(&p->fds, FD_EPOLL, ep, O_RDWR);
    if (fd < 0) {
        slab_free(&epoll_slab, ep);
        return -EMFILE;
    }
    return fd;
}

long do_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *epfde = fd_get(&p->fds, epfd);
    if (!epfde || epfde->type != FD_EPOLL) return -EBADF;
    epoll_t *ep = (epoll_t *)epfde->obj;
    if (!ep) return -EBADF;

    if (op != EPOLL_CTL_DEL) {
        fd_entry_t *tgt = fd_get(&p->fds, fd);
        if (!tgt || tgt->type == FD_NONE) return -EBADF;
    }

    mutex_lock(&ep->lock);

    switch (op) {
    case EPOLL_CTL_ADD: {
        if (!event) {
            mutex_unlock(&ep->lock);
            return -EFAULT;
        }
        struct epoll_event kev;
        { int r = copy_from_user(&kev, event, sizeof(kev));
          if (r) { mutex_unlock(&ep->lock); return r; } }
        for (int i = 0; i < ep->count; i++) {
            if (ep->entries[i].fd == fd) {
                mutex_unlock(&ep->lock);
                return -EEXIST;
            }
        }
        if (ep->count >= EPOLL_MAX_FDS) {
            mutex_unlock(&ep->lock);
            return -ENOMEM;
        }
        ep->entries[ep->count].fd     = fd;
        ep->entries[ep->count].events = kev.events;
        ep->entries[ep->count].data   = kev.data;
        ep->entries[ep->count].et_armed = 1;
        ep->entries[ep->count].et_last  = 0;
        ep->count++;
        break;
    }
    case EPOLL_CTL_MOD: {
        if (!event) {
            mutex_unlock(&ep->lock);
            return -EFAULT;
        }
        struct epoll_event kev;
        { int r = copy_from_user(&kev, event, sizeof(kev));
          if (r) { mutex_unlock(&ep->lock); return r; } }
        int found = 0;
        for (int i = 0; i < ep->count; i++) {
            if (ep->entries[i].fd == fd) {
                ep->entries[i].events = kev.events;
                ep->entries[i].data   = kev.data;
                ep->entries[i].et_armed = 1;
                ep->entries[i].et_last  = 0;
                found = 1;
                break;
            }
        }
        if (!found) {
            mutex_unlock(&ep->lock);
            return -ENOENT;
        }
        break;
    }
    case EPOLL_CTL_DEL: {
        int found = -1;
        for (int i = 0; i < ep->count; i++) {
            if (ep->entries[i].fd == fd) { found = i; break; }
        }
        if (found < 0) {
            mutex_unlock(&ep->lock);
            return -ENOENT;
        }
        ep->entries[found] = ep->entries[ep->count - 1];
        ep->count--;
        break;
    }
    default:
        mutex_unlock(&ep->lock);
        return -EINVAL;
    }

    mutex_unlock(&ep->lock);
    return 0;
}

long do_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
    if (maxevents <= 0) return -EINVAL;
    if (!events || !user_ok((uint64_t)events, (size_t)maxevents * sizeof(*events)))
        return -EFAULT;

    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *epfde = fd_get(&p->fds, epfd);
    if (!epfde || epfde->type != FD_EPOLL) return -EBADF;
    epoll_t *ep = (epoll_t *)epfde->obj;
    if (!ep) return -EBADF;

    thread_t *ct = thread_current();
    uint64_t deadline_tsc;
    int infinite;
    if (ct && ct->wake_at_tsc && timeout > 0) {
        deadline_tsc = ct->wake_at_tsc;
        infinite = 0;
    } else {
        deadline_tsc = (timeout < 0)  ? 0
                     : (timeout == 0) ? 0
                     : timer_deadline_tsc((uint64_t)timeout);
        infinite = (timeout < 0);
    }

    for (;;) {
        mutex_lock(&ep->lock);

        int nready = 0;
        for (int i = 0; i < ep->count && nready < maxevents; i++) {
            epoll_entry_t *ent = &ep->entries[i];
            uint32_t interest = ent->events & ~EPOLLET;
            uint32_t r = fd_poll_readiness(ent->fd, interest);
            if (r) {
                struct epoll_event ev;
                ev.events = r & interest;
                ev.events |= r & (EPOLLHUP | EPOLLERR);
                if (ev.events) {
                    if (ent->events & EPOLLET) {
                        uint32_t new_bits = ev.events & ~ent->et_last;
                        if (!new_bits) continue;
                        ev.events = new_bits;
                        ent->et_last = ev.events | ent->et_last;
                    }
                    ev.data = ent->data;
                    copy_to_user(&events[nready], &ev, sizeof(ev));
                    nready++;
                }
            } else {
                if (ent->events & EPOLLET)
                    ent->et_last = 0;
            }
        }

        mutex_unlock(&ep->lock);

        if (nready > 0) { ct->wake_at_tsc = 0; ct->wake_at = 0; return nready; }
        if (timeout == 0) return 0;
        if (!infinite && timer_tsc_now() >= deadline_tsc) { ct->wake_at_tsc = 0; ct->wake_at = 0; return 0; }

        {
            thread_t *t = thread_current();
            if (!t) return -EFAULT;
            t->wake_at_tsc = infinite ? 0 : deadline_tsc;
            t->wake_at = 0;
            epoll_sleeper_add_ext(t);
            int timeout_ms;
            if (infinite) {
                timeout_ms = -1;
            } else {
                uint64_t now_tsc = timer_tsc_now();
                if (now_tsc >= deadline_tsc) { t->wake_at_tsc = 0; return 0; }
                timeout_ms = (int)((deadline_tsc - now_tsc) / timer_tsc_per_ms);
                if (timeout_ms <= 0) timeout_ms = 1;
            }
            event_t ev;
            int _wr = event_wait(&t->eq, &ev, timeout_ms);
            if (_wr == -4) return -EINTR;
        }
    }
}

void epoll_incref(void *obj) {
    if (!obj) return;
    epoll_t *ep = (epoll_t *)obj;
    __sync_add_and_fetch(&ep->refcount, 1);
}

void epoll_destroy(void *obj) {
    if (!obj) return;
    epoll_t *ep = (epoll_t *)obj;
    if (__sync_sub_and_fetch(&ep->refcount, 1) <= 0)
        slab_free(&epoll_slab, obj);
}
