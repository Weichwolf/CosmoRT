/* CosmoRT eventfd — lightweight inter-thread signalling */

#include "event/epoll.h"
#include "proc/process.h"
#include "mm/slab.h"
#include "event/fd.h"

#include "uaccess.h"

#define EVENTFD_POOL_MAX 32

static eventfd_t eventfd_pool[EVENTFD_POOL_MAX];
static slab_t    eventfd_slab;

void eventfd_init(void) {
    slab_init(&eventfd_slab, eventfd_pool, (int)sizeof(eventfd_t), EVENTFD_POOL_MAX);
}

long do_eventfd2(unsigned int initval, int flags) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    eventfd_t *efd = (eventfd_t *)slab_alloc(&eventfd_slab);
    if (!efd) return -ENOMEM;

    efd->counter = initval;
    efd->flags = flags;
    efd->refcount = 1;
    efd->lock = (mutex_t)MUTEX_INIT;

    int fd_flags = O_RDWR;
    if (flags & EFD_CLOEXEC)  fd_flags |= O_CLOEXEC;
    if (flags & EFD_NONBLOCK) fd_flags |= O_NONBLOCK;

    int fd = fd_alloc(&p->fds, FD_EVENTFD, efd, fd_flags);
    if (fd < 0) {
        slab_free(&eventfd_slab, efd);
        return -EMFILE;
    }
    return fd;
}

long eventfd_read(void *obj, void *buf, long count) {
    if (count < 8) return -EINVAL;
    eventfd_t *efd = (eventfd_t *)obj;
    if (!efd) return -EBADF;

    mutex_lock(&efd->lock);
    if (efd->counter == 0) {
        mutex_unlock(&efd->lock);
        return -EAGAIN;
    }
    uint64_t val = efd->counter;
    efd->counter = 0;
    mutex_unlock(&efd->lock);

    copy_to_user(buf, &val, sizeof(val));
    return (long)sizeof(val);
}

long eventfd_write(void *obj, const void *buf, long count) {
    if (count < 8) return -EINVAL;
    eventfd_t *efd = (eventfd_t *)obj;
    if (!efd) return -EBADF;

    uint64_t val;
    copy_from_user(&val, buf, sizeof(val));

    mutex_lock(&efd->lock);
    if (efd->counter > 0xFFFFFFFFFFFFFFFEULL - val) {
        mutex_unlock(&efd->lock);
        return -EAGAIN;
    }
    efd->counter += val;
    mutex_unlock(&efd->lock);

    epoll_wake_all();
    return (long)sizeof(val);
}

void eventfd_destroy(void *obj) {
    if (!obj) return;
    eventfd_t *efd = (eventfd_t *)obj;
    if (__sync_sub_and_fetch(&efd->refcount, 1) <= 0)
        slab_free(&eventfd_slab, obj);
}
