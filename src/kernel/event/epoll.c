/* CosmoRT epoll — event multiplexing (Linux-compatible)
 *
 * Each epitem registers its own wait_queue_entry (poll_entry) on the source
 * fd's wq with func = ep_poll_callback. When the source fires wake_up, it
 * iterates its wq and calls each entry's func — for an epitem entry that
 * means: append epi to ep->rdllist (under ep->lock) and wake_up(&ep->wq).
 * epoll_wait blocks on ep->wq via DEFINE_WAIT and re-scans the entry list
 * (level-trigger semantics + ET edge tracking unchanged).
 *
 * Sources without a wq (FD_PTY_*, FD_INOTIFY, FD_SERIAL, FD_FILE) get no
 * push-wakeup; level-triggered epoll_wait still works because every wait
 * iteration re-scans fd_poll_readiness. Edge-trigger on those sources will
 * miss spontaneous events — wire them up with their own wq in later phases.
 */

#include <stddef.h>

#include "event/epoll.h"
#include "sys/syscall.h"
#include "proc/process.h"
#include "hw/serial.h"
#include "mm/slab.h"
#include "spinlock.h"
#include "core/timer.h"
#include "event/fd.h"
#include "config.h"
#include "memops.h"
#include "core/waitqueue.h"

/* User-pointer validation + copy helpers */
#include "uaccess.h"

/* ── Epoll internals ─────────────────────────────── */

/* epitem — per-watch entry. Lives in epoll_entry_slab.
 * poll_entry hooks into source-fd wq with func=ep_poll_callback.
 * rdl_next chains epitems already on ep->rdllist. */
typedef struct epoll_entry {
    struct epoll_entry *next;       /* in ep->entries (registered watches) */
    struct epoll_entry *rdl_next;   /* in ep->rdllist (ready), 0 = not ready */
    int                rdl_on;      /* 1 = currently on rdllist */
    int                fd;
    int                src_type;    /* FD_*: cached for incref/decref symmetry */
    void              *src_obj;
    int                src_flags;
    uint32_t           events;      /* requested events incl EPOLLET/EPOLLONESHOT */
    uint64_t           data;
    uint32_t           et_last;     /* last reported readiness for edge detection */
    wait_queue_head_t *src_wq;      /* registered wq, NULL if source has none */
    wait_queue_entry_t poll_entry;  /* hook into src_wq */
    struct eventpoll  *ep;          /* back-pointer for ep_poll_callback */
} epoll_entry_t;

typedef struct eventpoll {
    epoll_entry_t    *entries;      /* head of watch list */
    epoll_entry_t    *rdllist;      /* head of ready list */
    int               count;
    int               refcount;
    spinlock_t        lock;         /* protects entries, rdllist */
    wait_queue_head_t wq;           /* epoll_wait sleepers */
} epoll_t;

static slab_t epoll_slab;
static slab_t epoll_entry_slab;

#define EPOLL_SLAB_INITIAL         8
#define EPOLL_ENTRY_SLAB_INITIAL  64

/* ── Init ────────────────────────────────────────── */

void epoll_init(void) {
    slab_init_dynamic(&epoll_slab,       (int)sizeof(epoll_t),       EPOLL_SLAB_INITIAL);
    slab_init_dynamic(&epoll_entry_slab, (int)sizeof(epoll_entry_t), EPOLL_ENTRY_SLAB_INITIAL);
    eventfd_init();
    timerfd_init();
    inotify_init_slab();
    serial_puts("epoll: init\n");
}

/* hrtimer fn for epoll_wait timeout: wake the parking thread directly via
 * sched_wake (state CAS BLOCKED→RUNNABLE + sched_add). The waiter then
 * detects timeout via hrtimer_now_ns() in its post-schedule check. */
extern void sched_wake(struct thread *t);
static void ep_wait_timeout_fn(hrtimer_t *t) {
    struct thread *th = (struct thread *)t->data;
    if (th) sched_wake(th);
}

/* ── ep_poll_callback ────────────────────────────── */

#define ep_entry_from_poll(e)                                         \
    ((epoll_entry_t *)((char *)(e) - offsetof(epoll_entry_t, poll_entry)))

/* Source-fd wq fires (e.g. eventfd_write -> wake_up(&efd->read_wq)).
 * Wake any thread blocked in epoll_wait on ep->wq. The actual readiness
 * scan happens in ep_scan_entries on the wake-side; ep_poll_callback's
 * job is only to signal "something on this ep changed". rdllist is an
 * accelerator: callback adds the firing epi to it so ep_scan_entries
 * can poll just the fired entries first, falling back to a full scan
 * for sources without wq. */
static int ep_poll_callback(wait_queue_entry_t *e, unsigned int mask) {
    epoll_entry_t *epi = ep_entry_from_poll(e);
    if (!epi || !epi->ep) return 0;
    epoll_t *ep = epi->ep;

    uint64_t irqf;
    spin_lock_irq(&ep->lock, &irqf);
    if (!epi->rdl_on) {
        epi->rdl_next = ep->rdllist;
        ep->rdllist = epi;
        epi->rdl_on = 1;
    }
    spin_unlock_irq(&ep->lock, irqf);

    wake_up(&ep->wq);
    (void)mask;
    return 1;
}

/* ── epoll_obj_wq — accessor for fd_get_poll_wq (nested epoll) ── */

wait_queue_head_t *epoll_obj_wq(void *obj) {
    if (!obj) return 0;
    return &((epoll_t *)obj)->wq;
}

/* ── SYS_EPOLL_CREATE1 (291) ────────────────────── */

long do_epoll_create1(int flags) {
    if (flags & ~EPOLL_CLOEXEC) return -EINVAL;
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    epoll_t *ep = (epoll_t *)slab_alloc(&epoll_slab);
    if (!ep) return -ENOMEM;

    ep->entries = 0;
    ep->rdllist = 0;
    ep->count = 0;
    ep->refcount = 1;
    ep->lock = (spinlock_t)SPINLOCK_INIT;
    init_waitqueue_head(&ep->wq);

    int fd_flags = O_RDWR;
    if (flags & EPOLL_CLOEXEC) fd_flags |= O_CLOEXEC;
    int fd = fd_alloc(p->fds, FD_EPOLL, ep, fd_flags);
    if (fd < 0) {
        slab_free(&epoll_slab, ep);
        return -EMFILE;
    }
    return fd;
}

/* ── EPOLL_CTL_ADD plumbing ──────────────────────── */

/* Subscribe epi->poll_entry on source-fd wq. Caller must NOT hold ep->lock
 * (add_wait_queue takes wq->lock; nested with ep->lock would invert order
 * versus the wake path which takes wq->lock first then ep->lock). */
static void ep_attach_source(epoll_entry_t *epi) {
    epi->poll_entry.task  = 0;   /* unused for ep_poll_callback */
    epi->poll_entry.flags = 0;
    epi->poll_entry.func  = ep_poll_callback;
    epi->poll_entry.next  = 0;
    epi->poll_entry.prev  = 0;
    if (epi->src_wq) add_wait_queue(epi->src_wq, &epi->poll_entry);
}

/* Reverse of ep_attach_source. Idempotent. */
static void ep_detach_source(epoll_entry_t *epi) {
    if (epi->src_wq) remove_wait_queue(epi->src_wq, &epi->poll_entry);
}

/* Take ep->lock + remove epi from rdllist if present. */
static void ep_rdl_remove(epoll_t *ep, epoll_entry_t *target) {
    uint64_t irqf;
    spin_lock_irq(&ep->lock, &irqf);
    if (target->rdl_on) {
        epoll_entry_t **pp = &ep->rdllist;
        while (*pp) {
            if (*pp == target) { *pp = target->rdl_next; break; }
            pp = &(*pp)->rdl_next;
        }
        target->rdl_on = 0;
        target->rdl_next = 0;
    }
    spin_unlock_irq(&ep->lock, irqf);
}

/* ── SYS_EPOLL_CTL (233) ────────────────────────── */

long do_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *epfde = fd_get(p->fds, epfd);
    if (!epfde || epfde->type != FD_EPOLL) return -EBADF;
    epoll_t *ep = (epoll_t *)epfde->obj;
    if (!ep) return -EBADF;

    /* Cannot add epoll to itself */
    if (fd == epfd) return -EINVAL;

    int tgt_type = FD_NONE;
    void *tgt_obj = 0;
    int tgt_flags = 0;

    /* Validate target fd exists (except for DEL, target may already be closed) */
    if (op != EPOLL_CTL_DEL) {
        fd_entry_t *tgt = fd_get(p->fds, fd);
        if (!tgt || tgt->type == FD_NONE) return -EBADF;

        /* Regular files and directories do not implement poll → EPERM (Linux) */
        if (tgt->type == FD_FILE) return -EPERM;

        tgt_type  = tgt->type;
        tgt_obj   = tgt->obj;
        tgt_flags = tgt->flags;

        /* Check epoll nesting depth + circular reference (ADD only) */
        if (op == EPOLL_CTL_ADD && tgt->type == FD_EPOLL) {
            #define EP_MAX_NESTS 5
            int depth = 0;
            epoll_t *walk = (epoll_t *)tgt->obj;
            while (walk && depth < EP_MAX_NESTS + 1) {
                depth++;
                epoll_t *next = 0;
                for (epoll_entry_t *it = walk->entries; it; it = it->next) {
                    if (it->fd == epfd) return -ELOOP;
                    fd_entry_t *e = fd_get(p->fds, it->fd);
                    if (e && e->type == FD_EPOLL && !next)
                        next = (epoll_t *)e->obj;
                }
                walk = next;
            }
            if (depth >= EP_MAX_NESTS) return -EINVAL;
        }
    }

    switch (op) {
    case EPOLL_CTL_ADD: {
        if (!event) return -EFAULT;
        struct epoll_event kev;
        { int r = copy_from_user(&kev, event, sizeof(kev)); if (r) return r; }

        uint64_t irqf;
        spin_lock_irq(&ep->lock, &irqf);
        for (epoll_entry_t *it = ep->entries; it; it = it->next) {
            if (it->fd == fd) { spin_unlock_irq(&ep->lock, irqf); return -EEXIST; }
        }
        spin_unlock_irq(&ep->lock, irqf);

        epoll_entry_t *ne = (epoll_entry_t *)slab_alloc(&epoll_entry_slab);
        if (!ne) return -ENOMEM;
        ne->fd        = fd;
        ne->src_type  = tgt_type;
        ne->src_obj   = tgt_obj;
        ne->src_flags = tgt_flags;
        ne->events    = kev.events;
        ne->data      = kev.data;
        ne->rdl_on    = 0;
        ne->rdl_next  = 0;
        ne->et_last   = 0;
        ne->ep        = ep;
        ne->src_wq    = fd_get_poll_wq(fd, kev.events);

        /* Attach BEFORE chaining so ep_poll_callback never sees a partial
         * epitem (priv/func set by ep_attach_source). */
        ep_attach_source(ne);

        spin_lock_irq(&ep->lock, &irqf);
        for (epoll_entry_t *it = ep->entries; it; it = it->next) {
            if (it->fd == fd) {
                spin_unlock_irq(&ep->lock, irqf);
                ep_detach_source(ne);
                slab_free(&epoll_entry_slab, ne);
                return -EEXIST;
            }
        }
        ne->next    = ep->entries;
        ep->entries = ne;
        ep->count++;
        spin_unlock_irq(&ep->lock, irqf);

        /* Bump source-object refcount so a close() of the source fd cannot
         * free the wq while our poll_entry is still hooked in. */
        fd_obj_incref(ne->src_type, ne->src_obj, ne->src_flags);

        /* Linux behaviour: if source is already ready at ADD time, queue
         * a wake so the next epoll_wait sees it without waiting for an edge. */
        if (fd_poll_readiness(fd, kev.events) & kev.events) {
            spin_lock_irq(&ep->lock, &irqf);
            if (!ne->rdl_on) {
                ne->rdl_next = ep->rdllist;
                ep->rdllist  = ne;
                ne->rdl_on   = 1;
            }
            spin_unlock_irq(&ep->lock, irqf);
            wake_up(&ep->wq);
        }
        return 0;
    }
    case EPOLL_CTL_MOD: {
        if (!event) return -EFAULT;
        struct epoll_event kev;
        { int r = copy_from_user(&kev, event, sizeof(kev)); if (r) return r; }

        uint64_t irqf;
        spin_lock_irq(&ep->lock, &irqf);
        epoll_entry_t *found = 0;
        for (epoll_entry_t *it = ep->entries; it; it = it->next) {
            if (it->fd == fd) { found = it; break; }
        }
        if (!found) { spin_unlock_irq(&ep->lock, irqf); return -ENOENT; }
        found->events  = kev.events;
        found->data    = kev.data;
        found->et_last = 0;
        spin_unlock_irq(&ep->lock, irqf);

        /* MOD may flip the side (EPOLLIN <-> EPOLLOUT) which changes the
         * source wq for split-wq sources (eventfd, pipe). Re-subscribe. */
        wait_queue_head_t *new_wq = fd_get_poll_wq(fd, kev.events);
        if (new_wq != found->src_wq) {
            ep_detach_source(found);
            found->src_wq = new_wq;
            ep_attach_source(found);
        }
        return 0;
    }
    case EPOLL_CTL_DEL: {
        uint64_t irqf;
        spin_lock_irq(&ep->lock, &irqf);
        epoll_entry_t **pp = &ep->entries;
        epoll_entry_t *victim = 0;
        while (*pp) {
            if ((*pp)->fd == fd) { victim = *pp; *pp = victim->next; break; }
            pp = &(*pp)->next;
        }
        if (!victim) { spin_unlock_irq(&ep->lock, irqf); return -ENOENT; }
        ep->count--;
        spin_unlock_irq(&ep->lock, irqf);

        ep_rdl_remove(ep, victim);
        ep_detach_source(victim);
        fd_cleanup_entry(victim->src_type, victim->src_obj, victim->src_flags);
        slab_free(&epoll_entry_slab, victim);
        return 0;
    }
    default:
        return -EINVAL;
    }
}

/* ── ep_send_events: scan all registered entries, copy ready ones to
 * user. Linux's fast-path scans only rdllist; we scan entries always
 * because not all our sources have a wq (FD_PTY_*, FD_INOTIFY,
 * FD_SERIAL, FD_FILE) and thus can't fire ep_poll_callback to populate
 * rdllist. The full scan is the level-trigger fallback. rdllist still
 * exists as a hint for ep_poll_callback to set "something changed" but
 * we don't depend on it being populated. ── */

static int ep_send_events(epoll_t *ep, struct epoll_event *uevents, int maxevents) {
    int nready = 0;

    uint64_t irqf;
    spin_lock_irq(&ep->lock, &irqf);

    for (epoll_entry_t *epi = ep->entries; epi && nready < maxevents; epi = epi->next) {
        uint32_t interest = epi->events & ~(EPOLLET | EPOLLONESHOT);
        if (!interest) continue;

        spin_unlock_irq(&ep->lock, irqf);
        uint32_t r = fd_poll_readiness(epi->fd, interest | EPOLLHUP | EPOLLERR | EPOLLRDHUP);
        spin_lock_irq(&ep->lock, &irqf);

        uint32_t evbits = (r & interest) | (r & (EPOLLHUP | EPOLLERR | EPOLLRDHUP));
        if (!evbits) {
            if (epi->events & EPOLLET) epi->et_last = 0;
            continue;
        }

        if (epi->events & EPOLLET) {
            uint32_t new_bits = evbits & ~epi->et_last;
            if (!new_bits) continue;
            evbits = new_bits;
            epi->et_last |= evbits;
        }

        struct epoll_event ev;
        ev.events = evbits;
        ev.data   = epi->data;
        spin_unlock_irq(&ep->lock, irqf);
        int cferr = copy_to_user(&uevents[nready], &ev, sizeof(ev));
        spin_lock_irq(&ep->lock, &irqf);
        if (cferr != 0) {
            spin_unlock_irq(&ep->lock, irqf);
            return nready > 0 ? nready : -EFAULT;
        }
        nready++;

        if (epi->events & EPOLLONESHOT)
            epi->events &= ~(EPOLLIN | EPOLLOUT | EPOLLRDHUP);

        /* rdllist hint cleared; ep_poll_callback re-adds on next source wake. */
        if (epi->rdl_on) {
            epoll_entry_t **pp = &ep->rdllist;
            while (*pp) {
                if (*pp == epi) { *pp = epi->rdl_next; break; }
                pp = &(*pp)->rdl_next;
            }
            epi->rdl_on   = 0;
            epi->rdl_next = 0;
        }
    }

    spin_unlock_irq(&ep->lock, irqf);
    return nready;
}

/* ── SYS_EPOLL_WAIT (232) ───────────────────────── */

long do_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
    if (maxevents <= 0) return -EINVAL;

    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *epfde = fd_get(p->fds, epfd);
    if (!epfde || epfde->type == FD_NONE) return -EBADF;
    if (epfde->type != FD_EPOLL) return -EINVAL;
    epoll_t *ep = (epoll_t *)epfde->obj;
    if (!ep) return -EINVAL;

    if (!events || !user_ok((uint64_t)events, (size_t)maxevents * sizeof(*events)))
        return -EFAULT;

    uint64_t deadline = (timeout <= 0) ? 0 : (timer_ms() + (uint64_t)timeout);
    int infinite = (timeout < 0);

    int n = ep_send_events(ep, events, maxevents);
    if (n != 0) return n;             /* >0: events delivered, <0: -EFAULT */
    if (timeout == 0) return 0;

    /* Block on ep->wq until ep_poll_callback adds something to rdllist
     * or signal arrives or deadline expires. We drive the loop ourselves
     * (instead of schedule_timeout_interruptible) because we need to
     * re-check rdllist on every wake — including non-timeout wakes from
     * ep_poll_callback. */
    DEFINE_WAIT(wait);
    long rc = 0;
    hrtimer_t timer;
    int has_timer = 0;
    uint64_t deadline_ns = 0;

    if (!infinite) {
        uint64_t now_ms = timer_ms();
        if (now_ms >= deadline) {
            return 0;
        }
        deadline_ns = hrtimer_now_ns() + (deadline - now_ms) * 1000000ULL;
        hrtimer_init(&timer, ep_wait_timeout_fn, thread_current());
        has_timer = 1;
    }

    for (;;) {
        prepare_to_wait(&ep->wq, &wait, /*THREAD_BLOCKED*/ 3);

        n = ep_send_events(ep, events, maxevents);
        if (n != 0) { rc = n; break; }
        if (signal_deliverable()) { rc = -EINTR; break; }
        if (has_timer && hrtimer_now_ns() >= deadline_ns) { rc = 0; break; }

        if (has_timer) hrtimer_start(&timer, deadline_ns);
        schedule();

        if (has_timer && hrtimer_now_ns() >= deadline_ns) {
            n = ep_send_events(ep, events, maxevents);
            rc = n;
            break;
        }
        if (signal_deliverable()) { rc = -EINTR; break; }
        /* Wake from ep_poll_callback or spurious — re-loop, re-scan entries. */
    }
    finish_wait(&ep->wq, &wait);
    if (has_timer) hrtimer_cancel(&timer);
    return rc;
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
        /* Wake any blocked epoll_wait callers; they re-check signal/deadline
         * and unwind. Then drop every epitem (detach + decref source). */
        wake_up_all(&ep->wq);

        epoll_entry_t *it = ep->entries;
        while (it) {
            epoll_entry_t *n = it->next;
            ep_detach_source(it);
            fd_cleanup_entry(it->src_type, it->src_obj, it->src_flags);
            slab_free(&epoll_entry_slab, it);
            it = n;
        }
        ep->entries = 0;
        ep->rdllist = 0;
        slab_free(&epoll_slab, obj);
    }
}
