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
#include "core/tick.h"

/* User-pointer validation + copy helpers */
#include "uaccess.h"
#include "arch/arch.h"

/* ── Epoll internals ─────────────────────────────── */

/* Per-watch entry: intrusive singly-linked. No systemwide cap; growth via
 * dynamic slab, bounded per-process by RLIMIT_NOFILE (target fds must exist). */
typedef struct epoll_entry {
    struct epoll_entry *next;
    int      fd;
    uint32_t events;     /* requested events (including EPOLLET flag) */
    uint64_t data;
    uint32_t et_armed;   /* edge-triggered: 1 = report next ready, 0 = already reported */
    uint32_t et_last;    /* last reported readiness bits (for true edge detection) */
} epoll_entry_t;

typedef struct {
    epoll_entry_t *entries;    /* head of watch list */
    int            count;
    int            refcount;
    spinlock_t     lock;
} epoll_t;

static slab_t epoll_slab;
static slab_t epoll_entry_slab;

#define EPOLL_SLAB_INITIAL         8
#define EPOLL_ENTRY_SLAB_INITIAL  64

/* ── Init ────────────────────────────────────────── */

static struct tick_callback epoll_timeout_cb;

void epoll_init(void) {
    slab_init_dynamic(&epoll_slab,       (int)sizeof(epoll_t),       EPOLL_SLAB_INITIAL);
    slab_init_dynamic(&epoll_entry_slab, (int)sizeof(epoll_entry_t), EPOLL_ENTRY_SLAB_INITIAL);
    eventfd_init();
    timerfd_init();
    inotify_init_slab();
    tick_register(&epoll_timeout_cb, epoll_check_timeouts, TICK_EVERY);
    serial_puts("epoll: init\n");
}

/* ── SYS_EPOLL_CREATE1 (291) ────────────────────── */

long do_epoll_create1(int flags) {
    if (flags & ~EPOLL_CLOEXEC) return -EINVAL;
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    epoll_t *ep = (epoll_t *)slab_alloc(&epoll_slab);
    if (!ep) return -ENOMEM;

    ep->count = 0;
    ep->refcount = 1;
    ep->lock = (spinlock_t)SPINLOCK_INIT;

    int fd_flags = O_RDWR;
    if (flags & EPOLL_CLOEXEC) fd_flags |= O_CLOEXEC;
    int fd = fd_alloc(&p->fds, FD_EPOLL, ep, fd_flags);
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

    /* Cannot add epoll to itself */
    if (fd == epfd) return -EINVAL;

    /* Validate target fd exists (except for DEL, target may already be closed) */
    if (op != EPOLL_CTL_DEL) {
        fd_entry_t *tgt = fd_get(&p->fds, fd);
        if (!tgt || tgt->type == FD_NONE) return -EBADF;

        /* Regular files and directories do not implement poll → EPERM (Linux) */
        if (tgt->type == FD_FILE) return -EPERM;

        /* Check epoll nesting depth + circular reference (ADD only) */
        if (op == EPOLL_CTL_ADD && tgt->type == FD_EPOLL) {
            /* Walk the chain: fd→ep2, check all entries of ep2 for epoll fds,
             * count depth. Linux max nesting = 5 (EP_MAX_NESTS). */
            #define EP_MAX_NESTS 5
            int depth = 0;
            epoll_t *walk = (epoll_t *)tgt->obj;
            /* BFS would be correct; simplified DFS chain check */
            while (walk && depth < EP_MAX_NESTS + 1) {
                depth++;
                /* Check if walk contains epfd → circular */
                epoll_t *next = 0;
                for (epoll_entry_t *it = walk->entries; it; it = it->next) {
                    if (it->fd == epfd) return -ELOOP;
                    fd_entry_t *e = fd_get(&p->fds, it->fd);
                    if (e && e->type == FD_EPOLL && !next)
                        next = (epoll_t *)e->obj;
                }
                walk = next;
            }
            if (depth >= EP_MAX_NESTS) return -EINVAL;
        }
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
        for (epoll_entry_t *it = ep->entries; it; it = it->next) {
            if (it->fd == fd) {
                spin_unlock_irq(&ep->lock, irqf);
                return -EEXIST;
            }
        }
        /* Allocate outside lock to avoid holding spinlock across page_alloc
         * growth path. Re-check duplicate after re-acquire. */
        spin_unlock_irq(&ep->lock, irqf);
        epoll_entry_t *ne = (epoll_entry_t *)slab_alloc(&epoll_entry_slab);
        if (!ne) return -ENOMEM;
        spin_lock_irq(&ep->lock, &irqf);
        for (epoll_entry_t *it = ep->entries; it; it = it->next) {
            if (it->fd == fd) {
                spin_unlock_irq(&ep->lock, irqf);
                slab_free(&epoll_entry_slab, ne);
                return -EEXIST;
            }
        }
        ne->fd        = fd;
        ne->events    = kev.events;
        ne->data      = kev.data;
        ne->et_armed  = 1;
        ne->et_last   = 0;
        ne->next      = ep->entries;
        ep->entries   = ne;
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
        epoll_entry_t *found = 0;
        for (epoll_entry_t *it = ep->entries; it; it = it->next) {
            if (it->fd == fd) { found = it; break; }
        }
        if (!found) {
            spin_unlock_irq(&ep->lock, irqf);
            return -ENOENT;
        }
        found->events   = kev.events;
        found->data     = kev.data;
        found->et_armed = 1;
        found->et_last  = 0;
        break;
    }
    case EPOLL_CTL_DEL: {
        epoll_entry_t **pp = &ep->entries;
        epoll_entry_t *victim = 0;
        while (*pp) {
            if ((*pp)->fd == fd) { victim = *pp; *pp = victim->next; break; }
            pp = &(*pp)->next;
        }
        if (!victim) {
            spin_unlock_irq(&ep->lock, irqf);
            return -ENOENT;
        }
        ep->count--;
        spin_unlock_irq(&ep->lock, irqf);
        slab_free(&epoll_entry_slab, victim);
        return 0;
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
    int cpu = percpu_self()->core_id;
    uint64_t irqf;
    spin_lock_irq(&core_sleepers[cpu].lock, &irqf);
    for (int i = 0; i < core_sleepers[cpu].count; i++) {
        if (core_sleepers[cpu].threads[i] == t) {
            spin_unlock_irq(&core_sleepers[cpu].lock, irqf);
            return;
        }
    }
    if (core_sleepers[cpu].count < EPOLL_SLEEPER_MAX)
        core_sleepers[cpu].threads[core_sleepers[cpu].count++] = t;
    spin_unlock_irq(&core_sleepers[cpu].lock, irqf);
}

static void epoll_sleeper_remove(thread_t *t) {
    int ncores = smp_num_cores();
    for (int cpu = 0; cpu < ncores; cpu++) {
        uint64_t irqf;
        spin_lock_irq(&core_sleepers[cpu].lock, &irqf);
        for (int i = 0; i < core_sleepers[cpu].count; i++) {
            if (core_sleepers[cpu].threads[i] == t) {
                core_sleepers[cpu].threads[i] =
                    core_sleepers[cpu].threads[--core_sleepers[cpu].count];
                i--;
            }
        }
        spin_unlock_irq(&core_sleepers[cpu].lock, irqf);
    }
}

void epoll_sleeper_add_ext(thread_t *t) { epoll_sleeper_add(t); }
void epoll_sleeper_remove_ext(thread_t *t) { epoll_sleeper_remove(t); }

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

    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *epfde = fd_get(&p->fds, epfd);
    if (!epfde || epfde->type == FD_NONE) return -EBADF;
    if (epfde->type != FD_EPOLL) return -EINVAL;
    epoll_t *ep = (epoll_t *)epfde->obj;
    if (!ep) return -EINVAL;

    /* EFAULT-Check nach EBADF/EINVAL (Linux-Reihenfolge).
     * user_ok prueft nur Adressbereich; fuer PROT_READ-only Pages schlaegt
     * erst der tatsaechliche write-Versuch (extable) fehl. Test-write byte 0
     * triggert den fault sofort, konsistent mit Linux access_ok + write. */
    if (!events || !user_ok((uint64_t)events, (size_t)maxevents * sizeof(*events)))
        return -EFAULT;
    {
        uint8_t probe = 0;
        if (copy_to_user(events, &probe, 1) != 0) return -EFAULT;
    }

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
        for (epoll_entry_t *ent = ep->entries; ent && nready < maxevents; ent = ent->next) {
            uint32_t interest = ent->events & ~(EPOLLET | EPOLLONESHOT);
            if (!interest) continue;
            /* Linux: HUP/ERR/RDHUP werden immer geliefert, auch wenn
             * nicht explizit in interest. Query maskiert also nur IN/OUT. */
            uint32_t r = fd_poll_readiness(ent->fd, interest | EPOLLHUP | EPOLLERR | EPOLLRDHUP);
            if (r) {
                struct epoll_event ev;
                ev.events = r & interest;
                ev.events |= r & (EPOLLHUP | EPOLLERR | EPOLLRDHUP);
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
                    if (ent->events & EPOLLONESHOT)
                        ent->events &= ~(EPOLLIN | EPOLLOUT | EPOLLRDHUP);
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
    if (__sync_sub_and_fetch(&ep->refcount, 1) <= 0) {
        epoll_entry_t *it = ep->entries;
        while (it) {
            epoll_entry_t *n = it->next;
            slab_free(&epoll_entry_slab, it);
            it = n;
        }
        ep->entries = 0;
        slab_free(&epoll_slab, obj);
    }
}
