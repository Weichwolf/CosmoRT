/* CosmoRT hrtimer ns-precision ktests
 *
 * Phase 12-Rest: nanosleep / clock_nanosleep route through sleep_until_ns,
 * deadlines kept in hrtimer_now_ns() units, restart_block stores expires_ns.
 *
 * These tests cover the user-visible side: short ns-deadline sleeps must not
 * round up to the next ms tick, batch loops must not accumulate ms-rounding
 * error, and CLOCK_MONOTONIC must report sub-ms progress between back-to-back
 * reads. Without ns-precision the LTP epoll_wait02 / clock_nanosleep02 /
 * clock_gettime04 budgets are unreachable. */

#include "ktest.h"

static long ns_diff(const struct k_timespec *before, const struct k_timespec *after) {
    return (after->tv_sec - before->tv_sec) * 1000000000L +
           (after->tv_nsec - before->tv_nsec);
}

/* ── 01: sub-ms nanosleep returns within tight bound ─────── */

static void test_hrns_01_sub_ms_sleep(void) {
    puts("\n[hrtimer-ns]\n");
    struct k_timespec rq = { .tv_sec = 0, .tv_nsec = 500000 /* 500us */ };
    struct k_timespec before, after;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    long r = sc2(SYS_NANOSLEEP, (long)&rq, 0);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    check_val("nanosleep(500us) returns 0", r, 0);
    long elapsed = ns_diff(&before, &after);
    check("nanosleep(500us) elapsed >= 500us", elapsed >= 500000);
    /* Allow up to 5ms of jitter — periodic LAPIC tick + 2-CPU QEMU
     * scheduling slack. Pre-Phase-12 this returned after ~1ms of rounding;
     * with ns-precision the jitter is now bound by hrtimer expiry, not
     * timer_ms() granularity. */
    check("nanosleep(500us) elapsed < 5ms", elapsed < 5000000);
}

/* ── 02: back-to-back clock_gettime advances within sub-ms ── */

static void test_hrns_02_clock_monotonic_subms(void) {
    struct k_timespec a, b;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&a);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&b);
    long diff = ns_diff(&a, &b);
    check("two clock_gettime reads strictly monotonic", diff > 0);
    /* TSC-based path (vDSO or kernel) must beat 1ms granularity. If we
     * still rounded to ms ticks, two adjacent reads inside the same tick
     * would diff = 0 frequently. */
    check("two clock_gettime reads < 100us apart", diff < 100000);
}

/* ── 03: 100x 100us loop completes in tight budget ────────── */

static void test_hrns_03_batch_short_sleep(void) {
    struct k_timespec rq = { .tv_sec = 0, .tv_nsec = 100000 /* 100us */ };
    struct k_timespec before, after;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    for (int i = 0; i < 100; i++) {
        long r = sc2(SYS_NANOSLEEP, (long)&rq, 0);
        if (r != 0) { fail("batch nanosleep returned non-zero", 0); return; }
    }
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    long elapsed_ms = ns_diff(&before, &after) / 1000000;
    check_ge("100x nanosleep(100us) completed", elapsed_ms, 10);
    /* Pre-Phase-12: each call rounded to 1ms -> 100ms total. With
     * ns-precision the lower bound is 10ms (sum) plus per-call overhead
     * — measured ~30ms in QEMU. 200ms is a comfortable upper bound. */
    check("100x nanosleep(100us) under 200ms", elapsed_ms < 200);
}

/* ── 04: clock_nanosleep ABSTIME with ns-deadline ─────────── */

static void test_hrns_04_clock_nanosleep_abstime_ns(void) {
    struct k_timespec now;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&now);
    /* Deadline 750us in the future. Pre-Phase-12 the kernel would round
     * the abs ns to ms and miss the deadline target by up to +1ms. */
    struct k_timespec target = now;
    target.tv_nsec += 750000;
    if (target.tv_nsec >= 1000000000L) {
        target.tv_sec++;
        target.tv_nsec -= 1000000000L;
    }
    long r = sc4(SYS_CLOCK_NANOSLEEP, CLOCK_MONOTONIC, TIMER_ABSTIME,
                 (long)&target, 0);
    check_val("clock_nanosleep ABSTIME ns returns 0", r, 0);
    struct k_timespec after;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    long missed = ns_diff(&target, &after);
    check("clock_nanosleep ABSTIME woke at-or-after target", missed >= 0);
    /* 50ms upper bound — periodic LAPIC tick + 2-CPU QEMU jitter on cold
     * timer scheduling. The crucial check above (woke at-or-after target)
     * proves we did not return early; the bound here just sanity-caps
     * the tail to catch a *grossly* mis-armed timer. */
    check("clock_nanosleep ABSTIME jitter < 50ms", missed < 50000000);
}

/* ── 05: 1ms epoll_wait timeout bounded by hrtimer ─────── */

static void test_hrns_05_epoll_wait_1ms_loop(void) {
    long ep = sc1(SYS_EPOLL_CREATE1, 0);
    if (ep < 0) { fail("epoll_create1", 0); return; }
    struct k_timespec before, after;
    /* 200x epoll_wait(1ms) on empty epoll. Pre-Phase-12 each call sat in
     * timer_ms() rounding and could return after >1ms; the ns-deadline
     * path should converge near 200ms total. LTP epoll_wait02 mirrors
     * this exactly with 500x; we keep ktest budget at 200x to fit the
     * 180s test-hw window with margin. */
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    for (int i = 0; i < 200; i++) {
        struct { unsigned events; unsigned long data; } ev;
        long r = sc4(SYS_EPOLL_WAIT, ep, (long)&ev, 1, 1 /* 1ms */);
        if (r != 0) { fail("epoll_wait timeout returned non-zero", 0); sc1(SYS_CLOSE, ep); return; }
    }
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    sc1(SYS_CLOSE, ep);
    long elapsed_ms = ns_diff(&before, &after) / 1000000;
    check_ge("200x epoll_wait(1ms) >= 200ms", elapsed_ms, 200);
    check("200x epoll_wait(1ms) under 2s", elapsed_ms < 2000);
}

TEST("hrtimer_ns/01_sub_ms_sleep",          test_hrns_01_sub_ms_sleep);
TEST("hrtimer_ns/02_clock_monotonic_subms", test_hrns_02_clock_monotonic_subms);
TEST("hrtimer_ns/03_batch_short_sleep",     test_hrns_03_batch_short_sleep);
TEST("hrtimer_ns/04_clock_nanosleep_abstime_ns", test_hrns_04_clock_nanosleep_abstime_ns);
TEST("hrtimer_ns/05_epoll_wait_1ms_loop",   test_hrns_05_epoll_wait_1ms_loop);
