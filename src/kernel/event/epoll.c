/* CosmoRT epoll — event multiplexing (Linux-compatible)
 *
 * Blocking: hlt-loop with IRQ wakeup, same pattern as do_poll in socket.c.
 */

#include "event/epoll.h"
#include "sys/syscall.h"
#include "proc/process.h"
#include "core/percpu.h"
#include "hw/serial.h"
#include "mm/slab.h"
#include "spinlock.h"
#include "core/timer.h"
#include "event/fd.h"
#include "config.h"
#include "memops.h"
#include "net/net.h"
#include "core/event_queue.h"

/* User-pointer validation + copy helpers */
#include "uaccess.h"
#include "arch/arch.h"

/* ── Epoll internals ─────────────────────────────── */

#define EPOLL_MAX_FDS   64
#define EPOLL_POOL_MAX  16

typedef struct {
    int      fd;
    uint32_t events;     /* requested events (including EPOLLET flag) */
    uint64_t data;
    uint32_t et_armed;   /* edge-triggered: 1 = report next ready, 0 = already reported */
    uint32_t et_last;    /* last reported readiness bits (for true edge detection) */
} epoll_entry_t;

typedef struct {
    epoll_entry_t entries[EPOLL_MAX_FDS];
    int           count;
    int           refcount;
    spinlock_t    lock;
} epoll_t;

static epoll_t epoll_pool[EPOLL_POOL_MAX];
static slab_t  epoll_slab;

/* ── Init ────────────────────────────────────────── */

void epoll_init(void) {
    slab_init(&epoll_slab, epoll_pool, (int)sizeof(epoll_t), EPOLL_POOL_MAX);
    eventfd_init();
    timerfd_init();
    inotify_init_slab();
    serial_puts("epoll: init\n");
}

/* ── SYS_EPOLL_CREATE1 (291) ────────────────────── */

long do_epoll_create1(int flags) {
    (void)flags;
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    epoll_t *ep = (epoll_t *)slab_alloc(&epoll_slab);
    if (!ep) return -ENOMEM;

    ep->count = 0;
    ep->refcount = 1;
    ep->lock = (spinlock_t)SPINLOCK_INIT;

    int fd = fd_alloc(&p->fds, FD_EPOLL, ep, O_RDWR);
    if (fd < 0) {
        slab_free(&epoll_slab, ep);
        return -EMFILE;
    }
    return fd;
}

/* ── SYS_EPOLL_CTL (233) ────────────────────────── */

long do_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *epfde = fd_get(&p->fds, epfd);
    if (!epfde || epfde->type != FD_EPOLL) return -EBADF;
    epoll_t *ep = (epoll_t *)epfde->obj;
    if (!ep) return -EBADF;

    /* Validate target fd exists (except for DEL, target may already be closed) */
    if (op != EPOLL_CTL_DEL) {
        fd_entry_t *tgt = fd_get(&p->fds, fd);
        if (!tgt || tgt->type == FD_NONE) return -EBADF;
    }

    uint64_t irqf;
    spin_lock_irq(&ep->lock, &irqf);

    switch (op) {
    case EPOLL_CTL_ADD: {
        if (!event) {
            spin_unlock_irq(&ep->lock, irqf);
            return -EFAULT;
        }
        struct epoll_event kev;
        { int r = copy_from_user(&kev, event, sizeof(kev));
          if (r) { spin_unlock_irq(&ep->lock, irqf); return r; } }
        for (int i = 0; i < ep->count; i++) {
            if (ep->entries[i].fd == fd) {
                spin_unlock_irq(&ep->lock, irqf);
                return -EEXIST;
            }
        }
        if (ep->count >= EPOLL_MAX_FDS) {
            spin_unlock_irq(&ep->lock, irqf);
            return -ENOMEM;
        }
        ep->entries[ep->count].fd     = fd;
        ep->entries[ep->count].events = kev.events;
        ep->entries[ep->count].data   = kev.data;
        ep->entries[ep->count].et_armed = 1; /* report on first ready */
        ep->entries[ep->count].et_last  = 0;
        ep->count++;
        break;
    }
    case EPOLL_CTL_MOD: {
        if (!event) {
            spin_unlock_irq(&ep->lock, irqf);
            return -EFAULT;
        }
        struct epoll_event kev;
        { int r = copy_from_user(&kev, event, sizeof(kev));
          if (r) { spin_unlock_irq(&ep->lock, irqf); return r; } }
        int found = 0;
        for (int i = 0; i < ep->count; i++) {
            if (ep->entries[i].fd == fd) {
                ep->entries[i].events = kev.events;
                ep->entries[i].data   = kev.data;
                ep->entries[i].et_armed = 1; /* re-arm on MOD */
                ep->entries[i].et_last  = 0;
                found = 1;
                break;
            }
        }
        if (!found) {
            spin_unlock_irq(&ep->lock, irqf);
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
            spin_unlock_irq(&ep->lock, irqf);
            return -ENOENT;
        }
        ep->entries[found] = ep->entries[ep->count - 1];
        ep->count--;
        break;
    }
    default:
        spin_unlock_irq(&ep->lock, irqf);
        return -EINVAL;
    }

    spin_unlock_irq(&ep->lock, irqf);
    return 0;
}

/* ── Epoll sleeper list ──────────────────────────── */

#define EPOLL_SLEEPER_MAX 32

static struct {
    thread_t  *threads[EPOLL_SLEEPER_MAX];
    int        count;
    spinlock_t lock;
} epoll_sleepers = { .lock = SPINLOCK_INIT };

static void epoll_sleeper_add(thread_t *t) {
    /* Caller holds no lock; we take sleeper lock with IRQs off. */
    uint64_t irqf;
    spin_lock_irq(&epoll_sleepers.lock, &irqf);
    if (epoll_sleepers.count < EPOLL_SLEEPER_MAX)
        epoll_sleepers.threads[epoll_sleepers.count++] = t;
    spin_unlock_irq(&epoll_sleepers.lock, irqf);
}

/* External entry point for do_poll in socket.c */
void epoll_sleeper_add_ext(thread_t *t) { epoll_sleeper_add(t); }

/* Wake all blocked epoll/poll sleepers. IRQ-safe. */
void epoll_wake_all(void) {
    uint64_t irqf;
    spin_lock_irq(&epoll_sleepers.lock, &irqf);
    int n = epoll_sleepers.count;
    thread_t *wake[EPOLL_SLEEPER_MAX];
    for (int i = 0; i < n; i++) {
        wake[i] = epoll_sleepers.threads[i];
        epoll_sleepers.threads[i] = 0;
    }
    epoll_sleepers.count = 0;
    spin_unlock_irq(&epoll_sleepers.lock, irqf);

    extern void event_post(thread_t *target, uint32_t type, uint64_t data);
    for (int i = 0; i < n; i++)
        event_post(wake[i], 7 /* EQ_EPOLL_READY */, 0);
}

/* Check timed-out sleepers. Called from timer IRQ (sched_preempt). */
void epoll_check_timeouts(void) {
    uint64_t now = timer_ms();

    /* Also wake sleepers if any timerfd has expired — the timerfd
     * may be registered in their epoll set but the sleeper doesn't
     * know until they re-scan. */
    int need_wake = 0;
    if (timerfd_any_expired()) need_wake = 1;
    /* NIC packet wakeup removed — IRQ handler calls epoll_wake_all directly.
     * Timer-based wakeup caused spurious EPOLLIN on wrong sockets. */
    if (need_wake) epoll_wake_all();

    uint64_t irqf;
    spin_lock_irq(&epoll_sleepers.lock, &irqf);

    extern void event_post(thread_t *target, uint32_t type, uint64_t data);
    for (int i = 0; i < epoll_sleepers.count; ) {
        thread_t *t = epoll_sleepers.threads[i];
        if (!t) { /* Race: another CPU removed the thread */
            epoll_sleepers.threads[i] = epoll_sleepers.threads[--epoll_sleepers.count];
            continue;
        }
        if (t->wake_at && now >= t->wake_at) {
            /* Remove from list (swap with last) */
            epoll_sleepers.threads[i] = epoll_sleepers.threads[--epoll_sleepers.count];
            spin_unlock_irq(&epoll_sleepers.lock, irqf);
            event_post(t, 10 /* EQ_TIMEOUT */, 0);
            spin_lock_irq(&epoll_sleepers.lock, &irqf);
            /* Don't increment i — swapped element needs checking */
        } else {
            i++;
        }
    }
    spin_unlock_irq(&epoll_sleepers.lock, irqf);
}

/* ── SYS_EPOLL_WAIT (232) ───────────────────────── */

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

    /* Use absolute deadline to avoid timeout reset on re-execute.
     * On first call: compute deadline. On re-execute after wakeup:
     * check if the saved wake_at has passed. */
    thread_t *ct = thread_current();
    uint64_t deadline;
    int infinite;
    if (ct && ct->wake_at && timeout > 0) {
        /* Re-execute: use previously saved deadline */
        deadline = ct->wake_at;
        infinite = 0;
    } else {
        deadline = (timeout < 0)  ? 0
                 : (timeout == 0) ? 0
                 : (timer_ms() + (uint64_t)timeout);
        infinite = (timeout < 0);
    }

    for (;;) {
        /* Scan entries under lock. EPOLLET state must be updated atomically. */
        uint64_t irqf;
        spin_lock_irq(&ep->lock, &irqf);

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

        spin_unlock_irq(&ep->lock, irqf);

        if (nready > 0) { ct->wake_at = 0; return nready; }
        if (timeout == 0) return 0;
        if (!infinite && timer_ms() >= deadline) { ct->wake_at = 0; return 0; }

        /* No events ready — block via event_wait.
         * epoll_wake_all / epoll_check_timeouts will event_post us.
         * If event pre-queued (spurious), event_wait returns immediately
         * and we loop back to re-scan. */
        {
            thread_t *t = thread_current();
            if (!t) return -EFAULT;
            t->wake_at = infinite ? 0 : deadline;
            epoll_sleeper_add(t);
            int timeout_ms = infinite ? -1 : (int)(deadline - timer_ms());
            if (timeout_ms <= 0 && !infinite) { t->wake_at = 0; return 0; }
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
