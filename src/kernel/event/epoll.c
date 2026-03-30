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
#include "core/smp.h"

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

/* ── Per-core sleeper lists ──────────────────────── */
/* Each core has its own sleeper list + spinlock. No cross-core contention.
 * RT-Core (core 0) never acquires a Compute-Core's lock and vice versa.
 * When a thread migrates, its sleeper entry stays on the original core —
 * the timeout fires there and wakes via event_post/sched_wake (IPI-safe). */

#define EPOLL_SLEEPER_MAX 32

static struct {
    thread_t  *threads[EPOLL_SLEEPER_MAX];
    int        count;
    spinlock_t lock;
} core_sleepers[SMP_MAX_CORES] = {
    [0 ... SMP_MAX_CORES-1] = { .lock = SPINLOCK_INIT }
};

static void epoll_sleeper_add(thread_t *t) {
    /* Add to CURRENT core's list — no cross-core lock acquisition. */
    int cpu = percpu_self()->core_id;
    uint64_t irqf;
    spin_lock_irq(&core_sleepers[cpu].lock, &irqf);
    if (core_sleepers[cpu].count < EPOLL_SLEEPER_MAX)
        core_sleepers[cpu].threads[core_sleepers[cpu].count++] = t;
    spin_unlock_irq(&core_sleepers[cpu].lock, irqf);
}

/* External entry point for do_poll in socket.c */
void epoll_sleeper_add_ext(thread_t *t) { epoll_sleeper_add(t); }

/* Wake all blocked epoll/poll sleepers across ALL cores. IRQ-safe.
 * Rare path: called from IRQ handlers (NIC rx, pty write, eventfd, etc). */
void epoll_wake_all(void) {
    extern void event_post(thread_t *target, uint32_t type, uint64_t data);
    int ncores = smp_num_cores();

    for (int c = 0; c < ncores; c++) {
        uint64_t irqf;
        spin_lock_irq(&core_sleepers[c].lock, &irqf);
        int n = core_sleepers[c].count;
        thread_t *wake[EPOLL_SLEEPER_MAX];
        for (int i = 0; i < n; i++) {
            wake[i] = core_sleepers[c].threads[i];
            core_sleepers[c].threads[i] = 0;
        }
        core_sleepers[c].count = 0;
        spin_unlock_irq(&core_sleepers[c].lock, irqf);

        for (int i = 0; i < n; i++)
            event_post(wake[i], 7 /* EQ_EPOLL_READY */, 0);
    }
}

/* Check timed-out sleepers on CURRENT core only.
 * Called from timer IRQ (sched_preempt) on each core — each core
 * checks its own list, no shared lock across partitions.
 * timerfd expiry check runs only on BSP (core 0) since timerfd state is global. */
void epoll_check_timeouts(void) {
    uint64_t now_tsc = timer_tsc_now();
    int cpu = percpu_self()->core_id;

    /* timerfd wakeup: only BSP checks (global timerfd slab, avoids cross-core) */
    if (cpu == 0) {
        int need_wake = 0;
        if (timerfd_any_expired()) need_wake = 1;
        if (need_wake) epoll_wake_all();
    }

    uint64_t irqf;
    spin_lock_irq(&core_sleepers[cpu].lock, &irqf);

    extern void event_post(thread_t *target, uint32_t type, uint64_t data);
    for (int i = 0; i < core_sleepers[cpu].count; ) {
        thread_t *t = core_sleepers[cpu].threads[i];
        if (!t) {
            core_sleepers[cpu].threads[i] = core_sleepers[cpu].threads[--core_sleepers[cpu].count];
            continue;
        }
        /* TSC-based deadline comparison (sub-µs precision).
         * wake_at_tsc is authoritative; wake_at (ms) is legacy fallback. */
        uint64_t deadline = t->wake_at_tsc;
        if (!deadline && t->wake_at)
            deadline = timer_boot_tsc + t->wake_at * timer_tsc_per_ms;
        if (deadline && now_tsc >= deadline) {
            core_sleepers[cpu].threads[i] = core_sleepers[cpu].threads[--core_sleepers[cpu].count];
            spin_unlock_irq(&core_sleepers[cpu].lock, irqf);
            event_post(t, 10 /* EQ_TIMEOUT */, 0);
            spin_lock_irq(&core_sleepers[cpu].lock, &irqf);
        } else {
            i++;
        }
    }
    spin_unlock_irq(&core_sleepers[cpu].lock, irqf);
}

/* Return nearest TSC deadline among sleepers on the given core. 0 = none. */
uint64_t epoll_nearest_deadline_tsc(int core_id) {
    if (core_id < 0 || core_id >= SMP_MAX_CORES) return 0;
    uint64_t nearest = 0;
    uint64_t irqf;
    spin_lock_irq(&core_sleepers[core_id].lock, &irqf);
    for (int i = 0; i < core_sleepers[core_id].count; i++) {
        thread_t *t = core_sleepers[core_id].threads[i];
        if (!t) continue;
        uint64_t dl = t->wake_at_tsc;
        if (!dl && t->wake_at)
            dl = timer_boot_tsc + t->wake_at * timer_tsc_per_ms;
        if (dl && (!nearest || dl < nearest))
            nearest = dl;
    }
    spin_unlock_irq(&core_sleepers[core_id].lock, irqf);
    return nearest;
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

    /* Use absolute TSC deadline to avoid timeout reset on re-execute.
     * TSC-based: sub-ms precision, no timer_ms() on hot path. */
    thread_t *ct = thread_current();
    uint64_t deadline_tsc;
    int infinite;
    if (ct && ct->wake_at_tsc && timeout > 0) {
        /* Re-execute: use previously saved TSC deadline */
        deadline_tsc = ct->wake_at_tsc;
        infinite = 0;
    } else {
        deadline_tsc = (timeout < 0)  ? 0
                     : (timeout == 0) ? 0
                     : timer_deadline_tsc((uint64_t)timeout);
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

        if (nready > 0) { ct->wake_at_tsc = 0; ct->wake_at = 0; return nready; }
        if (timeout == 0) return 0;
        if (!infinite && timer_tsc_now() >= deadline_tsc) { ct->wake_at_tsc = 0; ct->wake_at = 0; return 0; }

        /* No events ready — block via event_wait.
         * epoll_wake_all / epoll_check_timeouts will event_post us.
         * If event pre-queued (spurious), event_wait returns immediately
         * and we loop back to re-scan. */
        {
            thread_t *t = thread_current();
            if (!t) return -EFAULT;
            t->wake_at_tsc = infinite ? 0 : deadline_tsc;
            t->wake_at = 0; /* TSC is authoritative */
            epoll_sleeper_add(t);
            /* Compute remaining ms for event_wait timeout (coarse, just for fallback) */
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
