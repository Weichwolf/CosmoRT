/* CosmoRT eventfd — lightweight inter-thread signalling */

#include "event/epoll.h"
#include "proc/process.h"
#include "proc/thread.h"
#include "mm/slab.h"
#include "spinlock.h"
#include "event/fd.h"
#include "core/waitqueue.h"

/* User-pointer validation + copy helpers */
#include "uaccess.h"

/* ── Eventfd pool ────────────────────────────────── */

#define EVENTFD_POOL_MAX 32

static eventfd_t eventfd_pool[EVENTFD_POOL_MAX];
static slab_t    eventfd_slab;

#define EVENTFD_MAX_VAL 0xFFFFFFFFFFFFFFFEULL

void eventfd_init(void) {
    slab_init(&eventfd_slab, eventfd_pool, (int)sizeof(eventfd_t), EVENTFD_POOL_MAX);
}

/* ── SYS_EVENTFD2 (290) ─────────────────────────── */

long do_eventfd2(unsigned int initval, int flags) {
    /* Linux: flags = EFD_SEMAPHORE | EFD_CLOEXEC | EFD_NONBLOCK (jeweils ein Bit).
     * Alle anderen Bits → EINVAL. */
    const int valid = EFD_SEMAPHORE | EFD_CLOEXEC | EFD_NONBLOCK;
    if (flags & ~valid) return -EINVAL;

    process_t *p = proc_current();
    if (!p) return -EFAULT;

    eventfd_t *efd = (eventfd_t *)slab_alloc(&eventfd_slab);
    if (!efd) return -ENOMEM;

    efd->counter = initval;
    efd->flags = flags;
    efd->refcount = 1;
    efd->lock = (spinlock_t)SPINLOCK_INIT;
    init_waitqueue_head(&efd->read_wq);
    init_waitqueue_head(&efd->write_wq);

    /* EFD_CLOEXEC/EFD_NONBLOCK → fd flags (values match O_CLOEXEC/O_NONBLOCK) */
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

/* Try a read under lock. Returns sizeof(val) on success, -EAGAIN on empty. */
static long eventfd_try_read(eventfd_t *efd, uint64_t *out_val) {
    uint64_t irqf;
    spin_lock_irq(&efd->lock, &irqf);
    if (efd->counter == 0) {
        spin_unlock_irq(&efd->lock, irqf);
        return -EAGAIN;
    }
    if (efd->flags & EFD_SEMAPHORE) {
        *out_val = 1;
        efd->counter -= 1;
    } else {
        *out_val = efd->counter;
        efd->counter = 0;
    }
    spin_unlock_irq(&efd->lock, irqf);
    /* Counter just freed space → wake writers waiting for room. */
    wake_up(&efd->write_wq);
    return (long)sizeof(uint64_t);
}

long eventfd_read(void *obj, void *buf, long count, int nonblock) {
    if (count < 8) return -EINVAL;
    eventfd_t *efd = (eventfd_t *)obj;
    if (!efd) return -EBADF;

    uint64_t val = 0;

    /* Fast path: counter already non-zero. */
    long r = eventfd_try_read(efd, &val);
    if (r > 0) {
        copy_to_user(buf, &val, sizeof(val));
        return r;
    }
    if (nonblock) return -EAGAIN;

    /* Block on read_wq until eventfd_try_write wakes us or signal arrives.
     * prepare_to_wait serializes state-transition with the waker, no race
     * window between condition-check and sleep. */
    DEFINE_WAIT(wait);
    long rc = 0;
    for (;;) {
        prepare_to_wait(&efd->read_wq, &wait, /*THREAD_BLOCKED*/ 3);

        r = eventfd_try_read(efd, &val);
        if (r > 0) { rc = r; break; }

        if (signal_deliverable()) { rc = -EINTR; break; }

        schedule();
    }
    finish_wait(&efd->read_wq, &wait);

    if (rc < 0) return rc;
    copy_to_user(buf, &val, sizeof(val));
    return rc;
}

/* Try a write under lock. Returns sizeof(val) or -EAGAIN on overflow. */
static long eventfd_try_write(eventfd_t *efd, uint64_t val) {
    uint64_t irqf;
    spin_lock_irq(&efd->lock, &irqf);
    if (efd->counter > EVENTFD_MAX_VAL - val) {
        spin_unlock_irq(&efd->lock, irqf);
        return -EAGAIN;
    }
    efd->counter += val;
    spin_unlock_irq(&efd->lock, irqf);
    /* Data available → wake readers blocked on read_wq. */
    wake_up(&efd->read_wq);
    return (long)sizeof(uint64_t);
}

long eventfd_write(void *obj, const void *buf, long count, int nonblock) {
    if (count < 8) return -EINVAL;
    eventfd_t *efd = (eventfd_t *)obj;
    if (!efd) return -EBADF;

    uint64_t val;
    copy_from_user(&val, buf, sizeof(val));
    if (val == (uint64_t)-1) return -EINVAL;

    /* Fast path: room available. eventfd_try_write wakes read_wq;
     * registered ep_poll_callback fires from there to ep->wq. */
    long r = eventfd_try_write(efd, val);
    if (r > 0) return r;
    if (nonblock) return -EAGAIN;

    DEFINE_WAIT(wait);
    long rc = 0;
    for (;;) {
        prepare_to_wait(&efd->write_wq, &wait, /*THREAD_BLOCKED*/ 3);

        r = eventfd_try_write(efd, val);
        if (r > 0) { rc = r; break; }

        if (signal_deliverable()) { rc = -EINTR; break; }

        schedule();
    }
    finish_wait(&efd->write_wq, &wait);
    return rc;
}

void eventfd_destroy(void *obj) {
    if (!obj) return;
    eventfd_t *efd = (eventfd_t *)obj;
    if (__sync_sub_and_fetch(&efd->refcount, 1) <= 0) {
        /* Wake any blocked readers/writers; they re-check signal_deliverable
         * or counter on resume. Order: wake first, then free is safe because
         * refcount==0 means no fd table holds this anymore — only stack-local
         * wait_queue_entry_t in callers, which finish_wait dequeues. */
        wake_up_all(&efd->read_wq);
        wake_up_all(&efd->write_wq);
        slab_free(&eventfd_slab, obj);
    }
}
