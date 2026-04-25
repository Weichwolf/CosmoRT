/* CosmoRT eventfd — lightweight inter-thread signalling */

#include "event/epoll.h"
#include "proc/process.h"
#include "proc/thread.h"
#include "mm/slab.h"
#include "spinlock.h"
#include "event/fd.h"
#include "core/event_queue.h"
#include "core/waitqueue.h"

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
    init_waitqueue_head(&efd->wq);
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

/* Try a read under lock. Returns sizeof(val) on success, -EAGAIN on empty.
 * On success may release a blocked writer (writer-side migration deferred —
 * still uses event_post on the legacy blocked_writer slot). */
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

    /* Fast path: counter already non-zero. */
    long r = eventfd_try_read(efd, &val);
    if (r > 0) { copy_to_user(buf, &val, sizeof(val)); return r; }
    if (nonblock) return -EAGAIN;

    /* Block on efd->wq until a writer increments and wakes us, or signal.
     * prepare_to_wait sets state under wq->lock so wake_up_interruptible
     * (which also takes wq->lock) cannot miss our entry — eliminates the
     * legacy single-blocked_reader race. */
    DEFINE_WAIT(wait);
    long rc = 0;
    for (;;) {
        prepare_to_wait(&efd->wq, &wait, /*THREAD_BLOCKED*/ 3);

        r = eventfd_try_read(efd, &val);
        if (r > 0) { rc = 0; break; }

        if (signal_deliverable()) { rc = -EINTR; break; }

        schedule();
    }
    finish_wait(&efd->wq, &wait);

    if (rc != 0) return rc;
    copy_to_user(buf, &val, sizeof(val));
    return (long)sizeof(uint64_t);
}

/* Try a write under lock. Returns sizeof(val) or -EAGAIN on overflow.
 * On success wakes blocked readers via the per-eventfd waitqueue. */
static long eventfd_try_write(eventfd_t *efd, uint64_t val) {
    uint64_t irqf;
    spin_lock_irq(&efd->lock, &irqf);
    if (efd->counter > EVENTFD_MAX_VAL - val) {
        spin_unlock_irq(&efd->lock, irqf);
        return -EAGAIN;
    }
    efd->counter += val;
    spin_unlock_irq(&efd->lock, irqf);
    /* Wake all readers — non-EFD_SEMAPHORE: first wakee drains the counter
     * and others re-loop to find counter==0 and block again; EFD_SEMAPHORE:
     * each wakee decrements by 1, others spin until exhausted. Cheap, since
     * realistic eventfd usage has 1-2 readers. */
    wake_up_interruptible(&efd->wq);
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
    if (__sync_sub_and_fetch(&efd->refcount, 1) <= 0) {
        /* Broadcast EOF-style wake to any waiters before freeing storage.
         * Pre-existing fds keep refcount > 0 — destroy only fires once
         * refcount hits 0, i.e. nobody else holds a reference, so any
         * blocked reader is by definition gone. wake_up_all is paranoia
         * for racy concurrent close(). */
        wake_up_all(&efd->wq);
        slab_free(&eventfd_slab, obj);
    }
}
