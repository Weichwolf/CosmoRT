/* CosmoRT timerfd — timer as file descriptor */

#include "event/epoll.h"
#include "proc/process.h"
#include "mm/slab.h"
#include "core/timer.h"
#include "event/fd.h"

#include "uaccess.h"

#define TIMERFD_POOL_MAX 16

static timerfd_t timerfd_pool[TIMERFD_POOL_MAX];
static slab_t    timerfd_slab;

void timerfd_init(void) {
    slab_init(&timerfd_slab, timerfd_pool, (int)sizeof(timerfd_t), TIMERFD_POOL_MAX);
}

long do_timerfd_create(int clockid, int flags) {
    (void)clockid;
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    timerfd_t *tfd = (timerfd_t *)slab_alloc(&timerfd_slab);
    if (!tfd) return -ENOMEM;

    tfd->expire_ms = 0;
    tfd->interval_ms = 0;
    tfd->expirations = 0;
    tfd->expire_tsc = 0;
    tfd->interval_tsc = 0;
    tfd->armed = 0;
    tfd->flags = flags;
    tfd->refcount = 1;
    tfd->lock = (mutex_t)MUTEX_INIT;

    int fd_flags = O_RDWR;
    if (flags & TFD_CLOEXEC)  fd_flags |= O_CLOEXEC;
    if (flags & TFD_NONBLOCK) fd_flags |= O_NONBLOCK;

    int fd = fd_alloc(&p->fds, FD_TIMERFD, tfd, fd_flags);
    if (fd < 0) {
        slab_free(&timerfd_slab, tfd);
        return -EMFILE;
    }
    return fd;
}

static uint64_t ts_to_ms(const struct k_timespec *ts) {
    return (uint64_t)ts->tv_sec * 1000 + (uint64_t)ts->tv_nsec / 1000000;
}

static void ms_to_ts(uint64_t ms, struct k_timespec *ts) {
    ts->tv_sec = (long)(ms / 1000);
    ts->tv_nsec = (long)((ms % 1000) * 1000000);
}

long do_timerfd_settime(int fd, int tfd_flags,
                        const struct k_itimerspec *new_value,
                        struct k_itimerspec *old_value) {
    if (!new_value) return -EFAULT;

    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_TIMERFD) return -EBADF;
    timerfd_t *tfd = (timerfd_t *)fde->obj;
    if (!tfd) return -EBADF;

    struct k_itimerspec knew;
    { int r = copy_from_user(&knew, new_value, sizeof(knew)); if (r) return r; }

    mutex_lock(&tfd->lock);

    if (old_value) {
        struct k_itimerspec kold;
        if (tfd->armed) {
            uint64_t now = timer_ms();
            uint64_t remaining = (tfd->expire_ms > now) ? (tfd->expire_ms - now) : 0;
            ms_to_ts(remaining, &kold.it_value);
            ms_to_ts(tfd->interval_ms, &kold.it_interval);
        } else {
            kold.it_value.tv_sec = 0;
            kold.it_value.tv_nsec = 0;
            kold.it_interval.tv_sec = 0;
            kold.it_interval.tv_nsec = 0;
        }
        copy_to_user(old_value, &kold, sizeof(kold));
    }

    uint64_t val_ms = ts_to_ms(&knew.it_value);
    uint64_t int_ms = ts_to_ms(&knew.it_interval);

    if (val_ms == 0 && int_ms == 0) {
        tfd->armed = 0;
        tfd->expirations = 0;
        tfd->expire_tsc = 0;
        tfd->interval_tsc = 0;
    } else {
        if (tfd_flags & 1)
            tfd->expire_ms = val_ms;
        else
            tfd->expire_ms = timer_ms() + val_ms;
        tfd->interval_ms = int_ms;
        if (tfd_flags & 1)
            tfd->expire_tsc = timer_boot_tsc + val_ms * timer_tsc_per_ms;
        else
            tfd->expire_tsc = timer_tsc_now() + val_ms * timer_tsc_per_ms;
        tfd->interval_tsc = int_ms * timer_tsc_per_ms;
        tfd->expirations = 0;
        tfd->armed = 1;
    }

    mutex_unlock(&tfd->lock);
    return 0;
}

int timerfd_any_expired(void) {
    uint64_t now_tsc = timer_tsc_now();
    for (int i = 0; i < TIMERFD_POOL_MAX; i++) {
        timerfd_t *t = &timerfd_pool[i];
        if (t->armed && t->expire_tsc && now_tsc >= t->expire_tsc) return 1;
    }
    return 0;
}

long timerfd_read(void *obj, void *buf, long count) {
    if (count < 8) return -EINVAL;
    timerfd_t *tfd = (timerfd_t *)obj;
    if (!tfd) return -EBADF;

    mutex_lock(&tfd->lock);

    if (tfd->armed) {
        uint64_t now_tsc = timer_tsc_now();
        while (tfd->armed && tfd->expire_tsc && now_tsc >= tfd->expire_tsc) {
            tfd->expirations++;
            if (tfd->interval_tsc > 0) {
                tfd->expire_tsc += tfd->interval_tsc;
                tfd->expire_ms += tfd->interval_ms;
            } else {
                tfd->armed = 0;
                tfd->expire_tsc = 0;
            }
        }
    }

    if (tfd->expirations == 0) {
        mutex_unlock(&tfd->lock);
        return -EAGAIN;
    }

    uint64_t val = tfd->expirations;
    tfd->expirations = 0;
    mutex_unlock(&tfd->lock);

    copy_to_user(buf, &val, sizeof(val));
    return (long)sizeof(val);
}

void timerfd_destroy(void *obj) {
    if (!obj) return;
    timerfd_t *tfd = (timerfd_t *)obj;
    if (__sync_sub_and_fetch(&tfd->refcount, 1) <= 0)
        slab_free(&timerfd_slab, obj);
}
