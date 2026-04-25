/* CosmoRT eventfd — lightweight inter-thread signalling */

#include "event/epoll.h"
#include "proc/process.h"
#include "mm/slab.h"
#include "spinlock.h"
#include "event/fd.h"
#include "core/event_queue.h"

/* User-pointer validation + copy helpers */
#include "uaccess.h"

/* ── Eventfd pool ────────────────────────────────── */

#define EVENTFD_POOL_MAX 32

static eventfd_t eventfd_pool[EVENTFD_POOL_MAX];
static slab_t    eventfd_slab;

#define EVENTFD_MAX_VAL 0xFFFFFFFFFFFFFFFEULL

/* event_post type code: same as pipe wakes */
#define EQ_EVENTFD_READY  4

extern void event_post(thread_t *target, uint32_t type, uint64_t data);

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
    efd->blocked_reader = 0;
    efd->blocked_writer = 0;

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
    /* Counter just freed space → wake one blocked writer */
    thread_t *writer = efd->blocked_writer;
    efd->blocked_writer = 0;
    spin_unlock_irq(&efd->lock, irqf);
    if (writer) event_post(writer, EQ_EVENTFD_READY, 0);
    return (long)sizeof(uint64_t);
}

long eventfd_read(void *obj, void *buf, long count, int nonblock) {
    if (count < 8) return -EINVAL;
    eventfd_t *efd = (eventfd_t *)obj;
    if (!efd) return -EBADF;

    uint64_t val;
    for (;;) {
        long r = eventfd_try_read(efd, &val);
        if (r > 0) {
            copy_to_user(buf, &val, sizeof(val));
            return r;
        }
        if (nonblock) return -EAGAIN;

        thread_t *t = thread_current();
        if (!t) return -EAGAIN;

        /* Register as blocked reader, then re-check (avoids race with writer) */
        uint64_t irqf;
        spin_lock_irq(&efd->lock, &irqf);
        if (efd->counter > 0) {
            spin_unlock_irq(&efd->lock, irqf);
            continue;
        }
        efd->blocked_reader = t;
        spin_unlock_irq(&efd->lock, irqf);

        /* POSIX: signal pending → -EINTR before blocking */
        if (t->proc) {
            uint64_t deliverable = t->proc->sig_pending & ~t->sig_blocked;
            if (deliverable) {
                spin_lock_irq(&efd->lock, &irqf);
                if (efd->blocked_reader == t) efd->blocked_reader = 0;
                spin_unlock_irq(&efd->lock, irqf);
                return -EINTR;
            }
        }

        event_t ev;
        int wr = event_wait(&t->eq, &ev, -1);
        if (wr == -4) {
            spin_lock_irq(&efd->lock, &irqf);
            if (efd->blocked_reader == t) efd->blocked_reader = 0;
            spin_unlock_irq(&efd->lock, irqf);
            return -EINTR;
        }
    }
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
    thread_t *reader = efd->blocked_reader;
    efd->blocked_reader = 0;
    spin_unlock_irq(&efd->lock, irqf);
    if (reader) event_post(reader, EQ_EVENTFD_READY, 0);
    return (long)sizeof(uint64_t);
}

long eventfd_write(void *obj, const void *buf, long count, int nonblock) {
    if (count < 8) return -EINVAL;
    eventfd_t *efd = (eventfd_t *)obj;
    if (!efd) return -EBADF;

    uint64_t val;
    copy_from_user(&val, buf, sizeof(val));
    if (val == (uint64_t)-1) return -EINVAL;

    for (;;) {
        long r = eventfd_try_write(efd, val);
        if (r > 0) {
            epoll_wake_all();
            return r;
        }
        if (nonblock) return -EAGAIN;

        thread_t *t = thread_current();
        if (!t) return -EAGAIN;

        uint64_t irqf;
        spin_lock_irq(&efd->lock, &irqf);
        if (efd->counter <= EVENTFD_MAX_VAL - val) {
            spin_unlock_irq(&efd->lock, irqf);
            continue;
        }
        efd->blocked_writer = t;
        spin_unlock_irq(&efd->lock, irqf);

        if (t->proc) {
            uint64_t deliverable = t->proc->sig_pending & ~t->sig_blocked;
            if (deliverable) {
                spin_lock_irq(&efd->lock, &irqf);
                if (efd->blocked_writer == t) efd->blocked_writer = 0;
                spin_unlock_irq(&efd->lock, irqf);
                return -EINTR;
            }
        }

        event_t ev;
        int wr = event_wait(&t->eq, &ev, -1);
        if (wr == -4) {
            spin_lock_irq(&efd->lock, &irqf);
            if (efd->blocked_writer == t) efd->blocked_writer = 0;
            spin_unlock_irq(&efd->lock, irqf);
            return -EINTR;
        }
    }
}

void eventfd_destroy(void *obj) {
    if (!obj) return;
    eventfd_t *efd = (eventfd_t *)obj;
    if (__sync_sub_and_fetch(&efd->refcount, 1) <= 0)
        slab_free(&eventfd_slab, obj);
}
