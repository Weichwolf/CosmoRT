/* CosmoRT Syscall Layer — time and clock syscalls */

#include "internal.h"

/* ── SYS_clock_gettime (228) / SYS_clock_getres (229) ── */

/* struct k_timespec defined in epoll.h, k_timeval in internal.h */

long do_clock_gettime(int clk_id, struct k_timespec *tp) {
    if (!tp) return -EFAULT;
    struct k_timespec kts;
    uint64_t ms = timer_ms();
    kts.tv_sec = (long)(ms / 1000);
    kts.tv_nsec = (long)((ms % 1000) * 1000000);
    switch (clk_id) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE: {
        extern uint64_t rtc_epoch_sec;
        kts.tv_sec += (long)rtc_epoch_sec;
        break;
    }
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
        break;
    default:
        return -EINVAL;
    }
    { int r = copy_to_user(tp, &kts, sizeof(kts)); if (r) return r; }
    return 0;
}

long do_clock_getres(int clk_id, struct k_timespec *tp) {
    (void)clk_id;
    if (tp) {
        struct k_timespec kts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1ms */
        int r = copy_to_user(tp, &kts, sizeof(kts));
        if (r) return r;
    }
    return 0;
}

/* Preemptible nanosleep via event_wait. On syscall restart after wake,
 * nanosleep_deadline distinguishes timeout (done) from signal (EINTR). */
long do_nanosleep(const struct k_timespec *req, struct k_timespec *rem) {
    if (!req) return -EFAULT;
    if (rem && !user_ok((uint64_t)rem, 16)) return -EFAULT;

    thread_t *t = thread_current();
    if (!t) return -EFAULT;

    /* Restarted after block — check if deadline reached */
    if (t->nanosleep_deadline) {
        uint64_t now = timer_ms();
        uint64_t dl = t->nanosleep_deadline;
        t->nanosleep_deadline = 0;
        if (now >= dl)
            return 0; /* sleep completed */
        /* Woken early (signal) — write remaining time */
        if (rem) {
            uint64_t left_ms = dl - now;
            struct k_timespec krem = {
                .tv_sec = (long)(left_ms / 1000),
                .tv_nsec = (long)((left_ms % 1000) * 1000000)
            };
            int r2 = copy_to_user(rem, &krem, sizeof(krem));
            if (r2) return r2;
        }
        return -EINTR;
    }

    /* First call — compute deadline and block */
    struct k_timespec kreq;
    { int r = copy_from_user(&kreq, req, sizeof(kreq)); if (r) return r; }
    uint64_t ms = (uint64_t)kreq.tv_sec * 1000 + (uint64_t)(kreq.tv_nsec / 1000000);
    if (ms == 0) { ms = 1; }

    /* Check pending signals before blocking (POSIX -EINTR) */
    if (t->proc) {
        uint64_t deliverable = t->proc->sig_pending & ~t->sig_blocked;
        if (deliverable) return -EINTR;
    }

    t->nanosleep_deadline = timer_ms() + ms;
    thread_block_ms((int)(ms > (uint64_t)0x7FFFFFFF ? 0x7FFFFFFF : ms));
    /* thread_block_ms blocks → syscall restarts → re-enters do_nanosleep above */
    __builtin_unreachable();
}

long do_clock_nanosleep(int clk_id, int flags,
                               const struct k_timespec *req, struct k_timespec *rem) {
    if (rem && !user_ok((uint64_t)rem, 16)) return -EFAULT;
    if (flags & TIMER_ABSTIME) {
        if (!req) return -EFAULT;

        thread_t *t = thread_current();
        if (!t) return -EFAULT;

        /* Restarted after block — check deadline */
        if (t->nanosleep_deadline) {
            uint64_t now = timer_ms();
            uint64_t dl = t->nanosleep_deadline;
            t->nanosleep_deadline = 0;
            if (now >= dl)
                return 0;
            /* TIMER_ABSTIME: no remaining time written (POSIX) */
            return -EINTR;
        }

        struct k_timespec kreq;
        { int r = copy_from_user(&kreq, req, sizeof(kreq)); if (r) return r; }
        uint64_t target_ms = (uint64_t)kreq.tv_sec * 1000
                           + (uint64_t)(kreq.tv_nsec / 1000000);
        /* CLOCK_REALTIME absolute target is wall-clock; convert to uptime */
        if (clk_id == CLOCK_REALTIME || clk_id == CLOCK_REALTIME_COARSE) {
            extern uint64_t rtc_epoch_sec;
            uint64_t epoch_ms = rtc_epoch_sec * 1000;
            if (target_ms > epoch_ms)
                target_ms -= epoch_ms;
            else
                return 0; /* deadline in the past */
        }
        uint64_t now = timer_ms();
        if (target_ms <= now)
            return 0; /* already past */

        uint64_t delta = target_ms - now;

        if (t->proc) {
            uint64_t deliverable = t->proc->sig_pending & ~t->sig_blocked;
            if (deliverable) return -EINTR;
        }

        t->nanosleep_deadline = target_ms;
        thread_block_ms((int)(delta > (uint64_t)0x7FFFFFFF ? 0x7FFFFFFF : delta));
        __builtin_unreachable();
    }
    return do_nanosleep(req, rem);
}

long do_gettimeofday(struct k_timeval *tv, void *tz) {
    (void)tz;
    if (tv) {
        extern uint64_t rtc_epoch_sec;
        struct k_timeval ktv;
        uint64_t ms = timer_ms();
        ktv.tv_sec = (long)(ms / 1000 + rtc_epoch_sec);
        ktv.tv_usec = (long)((ms % 1000) * 1000);
        int r = copy_to_user(tv, &ktv, sizeof(ktv));
        if (r) return r;
    }
    return 0;
}
