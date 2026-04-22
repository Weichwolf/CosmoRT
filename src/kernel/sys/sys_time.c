/* CosmoRT Syscall Layer — time and clock syscalls */

#include "internal.h"

/* struct k_timespec defined in epoll.h, k_timeval in internal.h */

/* ── NTP / adjtimex state (Linux __kernel_old_timex layout, 208 bytes) ── */

struct k_timex {
    unsigned int modes;      /* 0   */
    long         offset;     /* 8   */
    long         freq;       /* 16  */
    long         maxerror;   /* 24  */
    long         esterror;   /* 32  */
    int          status;     /* 40  */
    long         constant;   /* 48  */
    long         precision;  /* 56  */
    long         tolerance;  /* 64  */
    long         time_sec;   /* 72  */
    long         time_usec;  /* 80  */
    long         tick;       /* 88  */
    long         ppsfreq;    /* 96  */
    long         jitter;     /* 104 */
    int          shift;      /* 112 */
    long         stabil;     /* 120 */
    long         jitcnt;     /* 128 */
    long         calcnt;     /* 136 */
    long         errcnt;     /* 144 */
    long         stbcnt;     /* 152 */
    int          tai;        /* 160 */
    int          __padding[11]; /* 164..208 */
};

_Static_assert(sizeof(struct k_timex) == 208,
               "k_timex must match Linux __kernel_old_timex (208 bytes)");

/* Persisted NTP state. Defaults: TICK_NOMINAL, no status, zero offsets. */
static struct ntp_state {
    long offset;    /* usec */
    long freq;      /* scaled ppm */
    long maxerror;  /* usec */
    long esterror;  /* usec */
    int  status;    /* STA_* */
    long constant;  /* pll time constant */
    long tick;      /* usec between ticks */
    int  tai;       /* TAI offset */
} g_ntp = {
    .tick = TIMEX_TICK_NOMINAL,
    .status = STA_UNSYNC,
};

/* ── SYS_clock_gettime (228) / SYS_clock_getres (229) ── */

/* Per-process/thread CPU time in nanoseconds from tick count.
 * TICKS_PER_SECOND = 1000 → 1 tick = 1ms = 1_000_000ns. */
static uint64_t proc_cpu_time_ns(void) {
    process_t *p = proc_current();
    return p ? p->cpu_time_ticks * NSEC_PER_MSEC : 0;
}

static uint64_t thread_cpu_time_ns(void) {
    /* Without per-thread accounting, thread time approximates process time.
     * Multi-thread processes share cpu_time_ticks; single-thread returns same. */
    return proc_cpu_time_ns();
}

long do_clock_gettime(int clk_id, struct k_timespec *tp) {
    if (!tp) return -EFAULT;
    struct k_timespec kts;
    uint64_t ms = timer_ms();
    kts.tv_sec = (long)(ms / MSEC_PER_SEC);
    kts.tv_nsec = (long)((ms % MSEC_PER_SEC) * NSEC_PER_MSEC);
    switch (clk_id) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE: {
        extern int64_t rtc_epoch_sec;
        kts.tv_sec += (long)rtc_epoch_sec;
        break;
    }
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
        break;
    case CLOCK_PROCESS_CPUTIME_ID: {
        uint64_t ns = proc_cpu_time_ns();
        kts.tv_sec = (long)(ns / NSEC_PER_SEC);
        kts.tv_nsec = (long)(ns % NSEC_PER_SEC);
        break;
    }
    case CLOCK_THREAD_CPUTIME_ID: {
        uint64_t ns = thread_cpu_time_ns();
        kts.tv_sec = (long)(ns / NSEC_PER_SEC);
        kts.tv_nsec = (long)(ns % NSEC_PER_SEC);
        break;
    }
    default:
        return -EINVAL;
    }
    { int r = copy_to_user(tp, &kts, sizeof(kts)); if (r) return r; }
    return 0;
}

long do_clock_getres(int clk_id, struct k_timespec *tp) {
    switch (clk_id) {
    case CLOCK_REALTIME: case CLOCK_REALTIME_COARSE:
    case CLOCK_MONOTONIC: case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE: case CLOCK_BOOTTIME:
    case CLOCK_PROCESS_CPUTIME_ID: case CLOCK_THREAD_CPUTIME_ID:
        break;
    default:
        return -EINVAL;
    }
    if (tp) {
        struct k_timespec kts = { .tv_sec = 0, .tv_nsec = NSEC_PER_MSEC };
        int r = copy_to_user(tp, &kts, sizeof(kts));
        if (r) return r;
    }
    return 0;
}

/* In-kernel nanosleep: block via schedule(), loop until deadline or signal. */
long do_nanosleep(const struct k_timespec *req, struct k_timespec *rem) {
    if (!req) return -EFAULT;
    if (rem && !user_ok((uint64_t)rem, 16)) return -EFAULT;

    struct k_timespec kreq;
    { int r = copy_from_user(&kreq, req, sizeof(kreq)); if (r) return r; }
    if (kreq.tv_nsec < 0 || kreq.tv_nsec >= NSEC_PER_SEC) return -EINVAL;
    if (kreq.tv_sec < 0) return -EINVAL;
    uint64_t ms = (uint64_t)kreq.tv_sec * MSEC_PER_SEC
                + (uint64_t)(kreq.tv_nsec / NSEC_PER_MSEC);
    if (ms == 0 && kreq.tv_nsec > 0) ms = 1;
    if (ms == 0) return 0;

    uint64_t deadline = timer_ms() + ms;

    while (timer_ms() < deadline) {
        thread_t *t = thread_current();
        if (t && t->proc) {
            uint64_t deliverable = (t->proc->sig_pending | t->sig_thread_pending) & ~t->sig_blocked;
            if (deliverable) {
                if (rem) {
                    uint64_t now = timer_ms();
                    uint64_t left = (deadline > now) ? deadline - now : 0;
                    struct k_timespec krem = {
                        .tv_sec = (long)(left / MSEC_PER_SEC),
                        .tv_nsec = (long)((left % MSEC_PER_SEC) * NSEC_PER_MSEC)
                    };
                    copy_to_user(rem, &krem, sizeof(krem));
                }
                return -EINTR;
            }
        }
        uint64_t remaining = deadline - timer_ms();
        if (remaining == 0) break;
        thread_block_ms((int)(remaining > (uint64_t)0x7FFFFFFF ? 0x7FFFFFFF : remaining));
    }
    return 0;
}

long do_clock_nanosleep(int clk_id, int flags,
                               const struct k_timespec *req, struct k_timespec *rem) {
    switch (clk_id) {
    case CLOCK_MONOTONIC:
    case CLOCK_REALTIME:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_REALTIME_COARSE:
    case CLOCK_BOOTTIME:
    case CLOCK_PROCESS_CPUTIME_ID:
        break;
    case CLOCK_THREAD_CPUTIME_ID:
        /* Linux returns ENOTSUP for thread CPUTIME sleep */
        return -ENOTSUP;
    default:
        return -EINVAL;
    }
    if (!req || !user_ok((uint64_t)req, sizeof(struct k_timespec))) return -EFAULT;
    struct k_timespec kreq_chk;
    { int r = copy_from_user(&kreq_chk, req, sizeof(kreq_chk)); if (r) return r; }
    if (kreq_chk.tv_nsec < 0 || kreq_chk.tv_nsec >= NSEC_PER_SEC) return -EINVAL;
    if (kreq_chk.tv_sec < 0) return -EINVAL;
    if (rem && !user_ok((uint64_t)rem, 16)) return -EFAULT;
    if (flags & TIMER_ABSTIME) {
        uint64_t target_ms = (uint64_t)kreq_chk.tv_sec * MSEC_PER_SEC
                           + (uint64_t)(kreq_chk.tv_nsec / NSEC_PER_MSEC);
        if (clk_id == CLOCK_REALTIME || clk_id == CLOCK_REALTIME_COARSE) {
            extern int64_t rtc_epoch_sec;
            int64_t epoch_ms = rtc_epoch_sec * MSEC_PER_SEC;
            int64_t rel_ms = (int64_t)target_ms - epoch_ms;
            if (rel_ms <= 0) return 0;
            target_ms = (uint64_t)rel_ms;
        }

        while (timer_ms() < target_ms) {
            thread_t *t = thread_current();
            if (t && t->proc) {
                uint64_t deliverable = (t->proc->sig_pending | t->sig_thread_pending) & ~t->sig_blocked;
                if (deliverable) return -EINTR;
            }
            uint64_t now = timer_ms();
            if (now >= target_ms) break;
            uint64_t delta = target_ms - now;
            thread_block_ms((int)(delta > (uint64_t)0x7FFFFFFF ? 0x7FFFFFFF : delta));
        }
        return 0;
    }
    return do_nanosleep(req, rem);
}

long do_gettimeofday(struct k_timeval *tv, void *tz) {
    (void)tz;
    if (tv) {
        extern int64_t rtc_epoch_sec;
        struct k_timeval ktv;
        uint64_t ms = timer_ms();
        ktv.tv_sec = (long)((int64_t)(ms / MSEC_PER_SEC) + rtc_epoch_sec);
        ktv.tv_usec = (long)((ms % MSEC_PER_SEC) * NSEC_PER_USEC);
        int r = copy_to_user(tv, &ktv, sizeof(ktv));
        if (r) return r;
    }
    return 0;
}

/* ── SYS_clock_settime (227) ── */

long do_clock_settime(int clk_id, const void *tp) {
    if (!tp) return -EFAULT;
    if (!user_ok((uint64_t)tp, sizeof(struct k_timespec))) return -EFAULT;
    switch (clk_id) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
        break;
    default:
        return -EINVAL;
    }
    struct k_timespec kts;
    int r = copy_from_user(&kts, tp, sizeof(kts));
    if (r) return r;
    if (kts.tv_nsec < 0 || kts.tv_nsec >= NSEC_PER_SEC) return -EINVAL;
    if (kts.tv_sec < 0) return -EINVAL;
    process_t *p = proc_current();
    if (p && p->euid != 0) return -EPERM;
    extern int64_t rtc_epoch_sec;
    int64_t uptime_sec = (int64_t)(timer_ms() / MSEC_PER_SEC);
    rtc_epoch_sec = (int64_t)kts.tv_sec - uptime_sec;
    return 0;
}

/* ── SYS_adjtimex (159) / SYS_clock_adjtime (305) ── */

/* Map NTP state to current k_timex snapshot. Fills current wall time. */
static void ntp_fill_timex(struct k_timex *out) {
    extern int64_t rtc_epoch_sec;
    uint64_t ms = timer_ms();
    out->modes     = 0;
    out->offset    = g_ntp.offset;
    out->freq      = g_ntp.freq;
    out->maxerror  = g_ntp.maxerror;
    out->esterror  = g_ntp.esterror;
    out->status    = g_ntp.status;
    out->constant  = g_ntp.constant;
    out->precision = 1;
    out->tolerance = TIMEX_MAXFREQ;
    out->time_sec  = (long)((int64_t)(ms / MSEC_PER_SEC) + rtc_epoch_sec);
    out->time_usec = (long)((ms % MSEC_PER_SEC) * NSEC_PER_USEC);
    out->tick      = g_ntp.tick;
    out->ppsfreq   = 0;
    out->jitter    = 0;
    out->shift     = 0;
    out->stabil    = 0;
    out->jitcnt    = 0;
    out->calcnt    = 0;
    out->errcnt    = 0;
    out->stbcnt    = 0;
    out->tai       = g_ntp.tai;
    for (int i = 0; i < 11; i++) out->__padding[i] = 0;
}

/* Apply SET fields from caller buffer into NTP state. Returns -errno or 0.
 * Linux ignores unknown mode bits below ADJ_ADJTIME (0x8000); explicit
 * SINGLESHOT/SS_READ are the only allowed ADJ_ADJTIME combinations. */
static long ntp_apply_modes(const struct k_timex *in) {
    unsigned int m = in->modes;

    /* Special ADJ_ADJTIME modes (adjtime-style): SINGLESHOT sets offset once,
     * SS_READ returns remaining. Neither persists like ADJ_OFFSET. */
    if (m & 0x8000) {
        if (m != ADJ_OFFSET_SINGLESHOT && m != ADJ_OFFSET_SS_READ)
            return -EINVAL;
        return 0;
    }

    /* Reject unknown mode bits (Linux uses ADJ_ALL|ADJ_TAI|ADJ_SETOFFSET|
     * ADJ_MICRO|ADJ_NANO|ADJ_OFFSET_SS_READ) */
    const unsigned int valid = ADJ_ALL | ADJ_TAI | ADJ_SETOFFSET |
                               ADJ_MICRO | ADJ_NANO;
    if (m & ~valid) return -EINVAL;

    if (m & ADJ_TICK) {
        if (in->tick < TIMEX_TICK_MIN || in->tick > TIMEX_TICK_MAX)
            return -EINVAL;
    }
    if (m & ADJ_FREQUENCY) {
        if (in->freq > TIMEX_MAXFREQ || in->freq < -TIMEX_MAXFREQ)
            return -EINVAL;
    }

    if (m & ADJ_OFFSET)     g_ntp.offset   = in->offset;
    if (m & ADJ_FREQUENCY)  g_ntp.freq     = in->freq;
    if (m & ADJ_MAXERROR)   g_ntp.maxerror = in->maxerror;
    if (m & ADJ_ESTERROR)   g_ntp.esterror = in->esterror;
    if (m & ADJ_STATUS) {
        /* Preserve read-only status bits; let writable bits through. */
        g_ntp.status = (g_ntp.status & STA_RONLY) | (in->status & ~STA_RONLY);
    }
    if (m & ADJ_TIMECONST)  g_ntp.constant = in->constant;
    if (m & ADJ_TICK)       g_ntp.tick     = in->tick;
    if (m & ADJ_TAI)        g_ntp.tai      = in->tai;
    if (m & ADJ_MICRO)      g_ntp.status &= ~STA_NANO;
    if (m & ADJ_NANO)       g_ntp.status |=  STA_NANO;

    if (m & ADJ_SETOFFSET) {
        extern int64_t rtc_epoch_sec;
        rtc_epoch_sec += in->time_sec;
    }
    return 0;
}

long do_clock_adjtime(int clk_id, void *tx) {
    if (!tx) return -EFAULT;
    if (clk_id != CLOCK_REALTIME) return -EINVAL;
    struct k_timex ktx;
    int r = copy_from_user(&ktx, tx, sizeof(ktx));
    if (r) return r;

    /* Any SET requires CAP_SYS_TIME (euid==0 in this single-user kernel).
     * Read-only (modes==0 or ADJ_OFFSET_SS_READ) is allowed to all. */
    unsigned int m = ktx.modes;
    int is_read_only = (m == 0) || (m == ADJ_OFFSET_SS_READ);
    if (!is_read_only) {
        process_t *p = proc_current();
        if (p && p->euid != 0) return -EPERM;
    }

    long err = ntp_apply_modes(&ktx);
    if (err) return err;

    ntp_fill_timex(&ktx);
    r = copy_to_user(tx, &ktx, sizeof(ktx));
    if (r) return r;

    /* Return clock state: TIME_OK unless leap scheduled, error otherwise. */
    if (g_ntp.status & STA_INS) return TIME_INS;
    if (g_ntp.status & STA_DEL) return TIME_DEL;
    if (g_ntp.status & STA_UNSYNC) return TIME_ERROR;
    return TIME_OK;
}
