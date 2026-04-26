/* CosmoRT timerfd — timer as file descriptor */

#include "event/epoll.h"
#include "proc/process.h"
#include "proc/thread.h"
#include "mm/slab.h"
#include "spinlock.h"
#include "core/timer.h"
#include "core/hrtimer.h"
#include "core/waitqueue.h"
#include "event/fd.h"

/* User-pointer validation + copy helpers */
#include "uaccess.h"

/* ── Timerfd pool ────────────────────────────────── */

#define TIMERFD_POOL_MAX 16

static timerfd_t timerfd_pool[TIMERFD_POOL_MAX];
static slab_t    timerfd_slab;

void timerfd_init(void) {
    slab_init(&timerfd_slab, timerfd_pool, (int)sizeof(timerfd_t), TIMERFD_POOL_MAX);
}

/* ── hrtimer expiry callback ─────────────────────
 *
 * Bumps expirations under tfd->lock and wakes blocked readers via the
 * waitqueue. For periodic timers, re-arms the hrtimer to the next
 * interval boundary; for one-shot timers, marks the tfd disarmed.
 * Runs in IRQ context; only takes tfd->lock and waitqueue lock. */
static void timerfd_expire_fn(hrtimer_t *t) {
    timerfd_t *tfd = (timerfd_t *)t->data;
    if (!tfd) return;

    uint64_t irqf;
    spin_lock_irq(&tfd->lock, &irqf);
    if (!tfd->armed) {
        spin_unlock_irq(&tfd->lock, irqf);
        return;
    }
    /* Catch up if multiple intervals slipped past since last fire */
    uint64_t now = timer_ms();
    while (tfd->armed && now >= tfd->expire_ms) {
        tfd->expirations++;
        if (tfd->interval_ms > 0)
            tfd->expire_ms += tfd->interval_ms;
        else
            tfd->armed = 0;
    }
    int rearm = tfd->armed;
    uint64_t next_ms = tfd->expire_ms;
    spin_unlock_irq(&tfd->lock, irqf);

    if (rearm) {
        /* timer_ms() and hrtimer_now_ns() share the monotonic epoch (TSC).
         * Compute remaining delta in ms, convert to ns for hrtimer. */
        uint64_t now_ms = timer_ms();
        uint64_t delta_ms = (next_ms > now_ms) ? (next_ms - now_ms) : 0;
        hrtimer_start(&tfd->timer, hrtimer_now_ns() + ms_to_ns(delta_ms));
        tfd->timer_armed = 1;
    } else {
        tfd->timer_armed = 0;
    }

    wake_up_interruptible(&tfd->wq);
}

/* ── SYS_TIMERFD_CREATE (283) ────────────────────── */

long do_timerfd_create(int clockid, int flags) {
    (void)clockid;
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    timerfd_t *tfd = (timerfd_t *)slab_alloc(&timerfd_slab);
    if (!tfd) return -ENOMEM;

    tfd->expire_ms = 0;
    tfd->interval_ms = 0;
    tfd->expirations = 0;
    tfd->armed = 0;
    tfd->flags = flags;
    tfd->refcount = 1;
    tfd->lock = (spinlock_t)SPINLOCK_INIT;
    init_waitqueue_head(&tfd->wq);
    hrtimer_init(&tfd->timer, timerfd_expire_fn, tfd);
    tfd->timer_armed = 0;

    /* TFD_CLOEXEC/TFD_NONBLOCK → fd flags (values match O_CLOEXEC/O_NONBLOCK) */
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

    /* Cancel any pending hrtimer first, outside tfd->lock. After cancel
     * returns, the callback (which acquires tfd->lock) cannot fire again.
     * This prevents a window where the callback bumps expirations between
     * our reset and the new arm. */
    hrtimer_cancel(&tfd->timer);

    uint64_t irqf;
    spin_lock_irq(&tfd->lock, &irqf);
    tfd->timer_armed = 0;

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

    int rearm = 0;
    uint64_t deadline_ns = 0;

    if (val_ms == 0 && int_ms == 0) {
        tfd->armed = 0;
        tfd->expirations = 0;
    } else {
        uint64_t now_ms = timer_ms();
        if (tfd_flags & 1) /* TFD_TIMER_ABSTIME */
            tfd->expire_ms = val_ms;
        else
            tfd->expire_ms = now_ms + val_ms;
        tfd->interval_ms = int_ms;
        tfd->expirations = 0;
        tfd->armed = 1;
        uint64_t delta_ms = (tfd->expire_ms > now_ms) ? (tfd->expire_ms - now_ms) : 0;
        deadline_ns = hrtimer_now_ns() + ms_to_ns(delta_ms);
        rearm = 1;
    }

    tfd->timer_armed = rearm;
    spin_unlock_irq(&tfd->lock, irqf);

    if (rearm) hrtimer_start(&tfd->timer, deadline_ns);

    return 0;
}

/* Check if any timerfd has expired — utility for readiness scans. */
int timerfd_any_expired(void) {
    uint64_t now = timer_ms();
    for (int i = 0; i < TIMERFD_POOL_MAX; i++) {
        timerfd_t *t = &timerfd_pool[i];
        if (t->armed && now >= t->expire_ms) return 1;
    }
    return 0;
}

/* Drain expirations under lock: catch up the polled path (timer_ms advances
 * even if hrtimer_callback hasn't run yet), then read+clear the counter.
 * Returns expiration count, or 0 if nothing pending. */
static uint64_t timerfd_collect(timerfd_t *tfd) {
    uint64_t irqf;
    spin_lock_irq(&tfd->lock, &irqf);
    if (tfd->armed) {
        uint64_t now = timer_ms();
        while (tfd->armed && now >= tfd->expire_ms) {
            tfd->expirations++;
            if (tfd->interval_ms > 0)
                tfd->expire_ms += tfd->interval_ms;
            else
                tfd->armed = 0;
        }
    }
    uint64_t val = tfd->expirations;
    tfd->expirations = 0;
    spin_unlock_irq(&tfd->lock, irqf);
    return val;
}

long timerfd_read(void *obj, void *buf, long count, int nonblock) {
    if (count < 8) return -EINVAL;
    timerfd_t *tfd = (timerfd_t *)obj;
    if (!tfd) return -EBADF;

    /* Fast path: expirations already pending. */
    uint64_t val = timerfd_collect(tfd);
    if (val > 0) {
        copy_to_user(buf, &val, sizeof(val));
        return (long)sizeof(val);
    }

    if (nonblock) return -EAGAIN;

    /* Block on tfd->wq until hrtimer fires (wake_up_interruptible) or signal.
     * Loop re-checks under prepare_to_wait so a wake racing the condition
     * cannot be missed: waker holds wq->lock when transitioning state. */
    DEFINE_WAIT(wait);
    long rc = 0;
    for (;;) {
        prepare_to_wait(&tfd->wq, &wait, /*THREAD_BLOCKED*/ 3);

        val = timerfd_collect(tfd);
        if (val > 0) { rc = 0; break; }

        if (signal_deliverable()) { rc = -ERESTARTSYS; break; }

        /* Disarmed between fast-path and prepare_to_wait? Nothing will wake
         * us; return -EAGAIN-equivalent via signal-restart-or-cancelled.
         * Keep semantics consistent with Linux: if not armed, blocking read
         * sleeps until armed (no waker exists in CosmoRT to do that today),
         * so behave like an indefinite wait — let signal interrupt only. */
        schedule();
    }
    finish_wait(&tfd->wq, &wait);

    if (rc != 0) return rc;
    copy_to_user(buf, &val, sizeof(val));
    return (long)sizeof(val);
}

void timerfd_destroy(void *obj) {
    if (!obj) return;
    timerfd_t *tfd = (timerfd_t *)obj;
    if (__sync_sub_and_fetch(&tfd->refcount, 1) <= 0) {
        /* Cancel pending hrtimer + broadcast EOF-style wake to any blockers
         * before freeing storage. Order matters: cancel first so callback
         * cannot race the wake; wake_up_all unblocks readers that re-evaluate
         * and see armed=0/expirations=0 + signal_deliverable path. */
        hrtimer_cancel(&tfd->timer);
        wake_up_all(&tfd->wq);
        slab_free(&timerfd_slab, obj);
    }
}
