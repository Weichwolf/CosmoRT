/* CosmoRT Syscall Layer — time and clock syscalls */

#include "internal.h"
#include "core/time_ns.h"
#include "core/waitqueue.h"
#include "core/hrtimer.h"

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
    /* ns = (tsc - boot_tsc) * 1e6 / tsc_per_ms, same origin wie timer_ms.
     * Splitten um Overflow bei langer Uptime zu vermeiden. */
    uint64_t tsc_delta = timer_tsc_now() - timer_boot_tsc;
    uint64_t tpms = timer_tsc_per_ms ? timer_tsc_per_ms : 1;
    uint64_t ms = tsc_delta / tpms;
    uint64_t sub_tsc = tsc_delta - ms * tpms;
    uint64_t sub_ns = (sub_tsc * (uint64_t)NSEC_PER_MSEC) / tpms;
    int64_t raw_ns = (int64_t)ms * NSEC_PER_MSEC + (int64_t)sub_ns;
    switch (clk_id) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE: {
        extern int64_t rtc_epoch_sec;
        raw_ns += (int64_t)rtc_epoch_sec * NSEC_PER_SEC;
        break;
    }
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME: {
        /* time_namespace offset applied for affected clocks. */
        process_t *p = proc_current();
        if (p && p->time_ns)
            raw_ns = time_ns_adjust_read(p->time_ns, clk_id, raw_ns);
        break;
    }
    case CLOCK_PROCESS_CPUTIME_ID:
        raw_ns = (int64_t)proc_cpu_time_ns();
        break;
    case CLOCK_THREAD_CPUTIME_ID:
        raw_ns = (int64_t)thread_cpu_time_ns();
        break;
    default:
        return -EINVAL;
    }
    /* Normalise sec/nsec from raw_ns; raw_ns can be negative when a
     * time_ns offset is steeper than current uptime. */
    int64_t sec  = raw_ns / NSEC_PER_SEC;
    int64_t nsec = raw_ns - sec * NSEC_PER_SEC;
    if (nsec < 0) { sec -= 1; nsec += NSEC_PER_SEC; }
    kts.tv_sec  = (long)sec;
    kts.tv_nsec = (long)nsec;
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
        /* TSC-basierte Aufloesung: 1 ns (real), aber COARSE-Varianten
         * laufen weiter ueber timer_ms (1 ms). */
        long res_ns = (clk_id == CLOCK_REALTIME_COARSE ||
                       clk_id == CLOCK_MONOTONIC_COARSE) ? NSEC_PER_MSEC : 1;
        struct k_timespec kts = { .tv_sec = 0, .tv_nsec = res_ns };
        int r = copy_to_user(tp, &kts, sizeof(kts));
        if (r) return r;
    }
    return 0;
}

/* Common sleep loop: block until kernel-deadline_ns or signal.
 * deadline_ns is in hrtimer_now_ns() units (CLOCK_MONOTONIC ns since boot,
 * already adjusted for time_namespace and CLOCK_REALTIME epoch).
 * On interrupt returns -EINTR after writing remaining time to *out_left_ns.
 * On full sleep returns 0. */
static long sleep_until_ns(uint64_t deadline_ns, uint64_t *out_left_ns) {
    /* signal_deliverable filters SIG_DFL_IGNORE signals (SIGCHLD/SIGURG/
     * SIGWINCH/SIGIO with default handler) so spurious EINTR can not
     * livelock ERESTART_RESTARTBLOCK on a bit nobody clears. */
    for (;;) {
        uint64_t now = hrtimer_now_ns();
        if (now >= deadline_ns) {
            if (out_left_ns) *out_left_ns = 0;
            return 0;
        }
        if (signal_deliverable()) {
            if (out_left_ns) *out_left_ns = deadline_ns - now;
            return -EINTR;
        }
        uint64_t remaining_ns = deadline_ns - now;
        int r = sleep_interruptible_ns(remaining_ns);
        if (r == -EINTR) {
            now = hrtimer_now_ns();
            if (out_left_ns) *out_left_ns = (deadline_ns > now) ? deadline_ns - now : 0;
            return -EINTR;
        }
        /* r == 0 or -ETIMEDOUT: deadline reached or spurious wake; loop
         * re-checks now vs deadline and exits cleanly. */
    }
}

/* Write remaining time (ns) to userspace rem ptr. Returns -EFAULT on bad ptr. */
static int copy_rem_to_user(void *user_rmtp, uint64_t left_ns) {
    if (!user_rmtp) return 0;
    struct k_timespec krem = {
        .tv_sec  = (long)(left_ns / NSEC_PER_SEC),
        .tv_nsec = (long)(left_ns % NSEC_PER_SEC)
    };
    if (copy_to_user(user_rmtp, &krem, sizeof(krem)) < 0) return -EFAULT;
    return 0;
}

/* Convert TIMER_ABSTIME user deadline (in clk_id-space ns) to kernel
 * hrtimer_now_ns() deadline. Applies time_namespace offset and REALTIME
 * epoch shift. Stores nonzero in *expired when the deadline is in the
 * past; returned value is undefined in that case. */
static uint64_t abs_user_ns_to_kernel_ns(int clk_id, int64_t user_ns, int *expired) {
    int64_t target_ns = user_ns;
    if (time_ns_clock_affected(clk_id)) {
        process_t *cur_p = proc_current();
        if (cur_p && cur_p->time_ns)
            target_ns = time_ns_adjust_abs(cur_p->time_ns, clk_id, target_ns);
    }
    if (clk_id == CLOCK_REALTIME || clk_id == CLOCK_REALTIME_COARSE) {
        extern int64_t rtc_epoch_sec;
        int64_t epoch_ns = rtc_epoch_sec * (int64_t)NSEC_PER_SEC;
        int64_t rel_ns = target_ns - epoch_ns;
        if (rel_ns <= 0) { *expired = 1; return 0; }
        target_ns = rel_ns;
    }
    if (target_ns < 0) target_ns = 0;
    *expired = 0;
    return (uint64_t)target_ns;
}

/* Restart-block resume: continue the sleep until saved kernel-ns deadline.
 * Re-runs the sleep_until_ns loop. user_rmtp is updated on signal-EINTR
 * for relative-mode restarts; ABSTIME restarts have user_rmtp == NULL. */
static long clock_nanosleep_restart(struct restart_block *rb) {
    uint64_t deadline_ns = rb->nanosleep.expires_ns;
    if (hrtimer_now_ns() >= deadline_ns) return 0;
    uint64_t left_ns = 0;
    long r = sleep_until_ns(deadline_ns, &left_ns);
    if (r == -EINTR) {
        int werr = copy_rem_to_user(rb->nanosleep.user_rmtp, left_ns);
        if (werr) return werr;
        return -ERESTART_RESTARTBLOCK;
    }
    return r;
}

/* Arm thread->restart_block for clock_nanosleep_restart.
 * deadline_ns is the kernel hrtimer_now_ns() deadline (already adjusted for
 * time_ns / REALTIME epoch by the caller). After this call, return
 * -ERESTART_RESTARTBLOCK to enter the syscall-return restart path. */
static void arm_nanosleep_restart_block(int clk_id, int mode,
                                        uint64_t deadline_ns, void *user_rmtp) {
    thread_t *t = thread_current();
    if (!t) return;
    t->restart_block.fn = clock_nanosleep_restart;
    t->restart_block.nanosleep.clockid    = clk_id;
    t->restart_block.nanosleep.mode       = mode;
    t->restart_block.nanosleep.expires_ns = deadline_ns;
    t->restart_block.nanosleep.user_rmtp  = user_rmtp;
}

/* In-kernel nanosleep: block via schedule(), loop until deadline or signal. */
long do_nanosleep(const struct k_timespec *req, struct k_timespec *rem) {
    if (!req) return -EFAULT;
    if (rem && !user_ok((uint64_t)rem, 16)) return -EFAULT;

    struct k_timespec kreq;
    { int r = copy_from_user(&kreq, req, sizeof(kreq)); if (r) return r; }
    if (kreq.tv_nsec < 0 || kreq.tv_nsec >= NSEC_PER_SEC) return -EINVAL;
    if (kreq.tv_sec < 0) return -EINVAL;
    uint64_t ns = (uint64_t)kreq.tv_sec * NSEC_PER_SEC + (uint64_t)kreq.tv_nsec;
    if (ns == 0) return 0;

    uint64_t deadline_ns = hrtimer_now_ns() + ns;
    uint64_t left_ns = 0;
    long r = sleep_until_ns(deadline_ns, &left_ns);
    if (r == -EINTR) {
        /* EFAULT-on-rem outranks EINTR (Linux hrtimer_nanosleep behaviour). */
        int werr = copy_rem_to_user(rem, left_ns);
        if (werr) return werr;
        /* Save restart_block for SYS_restart_syscall. apply_restart()
         * converts -ERESTART_RESTARTBLOCK to -EINTR if a user handler
         * runs; otherwise re-dispatches NR=219 which calls our restart_fn. */
        arm_nanosleep_restart_block(CLOCK_MONOTONIC, 0, deadline_ns, rem);
        return -ERESTART_RESTARTBLOCK;
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
        int64_t user_abs_ns = (int64_t)kreq_chk.tv_sec * NSEC_PER_SEC
                            + (int64_t)kreq_chk.tv_nsec;
        int expired = 0;
        uint64_t deadline_ns = abs_user_ns_to_kernel_ns(clk_id, user_abs_ns, &expired);
        if (expired) return 0;
        long r = sleep_until_ns(deadline_ns, 0);
        if (r == -EINTR) {
            /* ABSTIME: rem buffer is unused (deadline is absolute). */
            arm_nanosleep_restart_block(clk_id, 1, deadline_ns, 0);
            return -ERESTART_RESTARTBLOCK;
        }
        return 0;
    }

    /* Relative: do_nanosleep handles req/rem directly. */
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
    /* Refresh user-visible wall-time offset so vDSO CLOCK_REALTIME tracks. */
    extern void vdso_data_update(void);
    vdso_data_update();
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
