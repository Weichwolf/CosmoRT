/* LTP clock_nanosleep01-04 — ported to ktest */
#include "ktest.h"

/* ── clock_nanosleep01: error conditions ── */

static void test_clock_nanosleep01(void) {
    puts("\n[ltp/clock_nanosleep01]\n");

    struct k_timespec rq, rm;

    /* EINVAL: negative tv_nsec */
    rq.tv_sec = 0; rq.tv_nsec = -1;
    rm.tv_sec = 0; rm.tv_nsec = 0;
    long r = sc4(SYS_CLOCK_NANOSLEEP, CLOCK_REALTIME, 0, (long)&rq, (long)&rm);
    check_val("negative tv_nsec EINVAL", r, -EINVAL);

    /* EINVAL: tv_nsec >= 1e9 */
    rq.tv_sec = 0; rq.tv_nsec = 1000000000;
    r = sc4(SYS_CLOCK_NANOSLEEP, CLOCK_REALTIME, 0, (long)&rq, (long)&rm);
    check_val("tv_nsec=1e9 EINVAL", r, -EINVAL);

    /* EINVAL: invalid clock id */
    rq.tv_sec = 0; rq.tv_nsec = 100;
    r = sc4(SYS_CLOCK_NANOSLEEP, -1, 0, (long)&rq, (long)&rm);
    check_val("invalid clock EINVAL", r, -EINVAL);
}

/* ── clock_nanosleep02: basic sleep — relative, verify elapsed >= requested ── */

static void test_clock_nanosleep02(void) {
    puts("\n[ltp/clock_nanosleep02]\n");

    struct k_timespec before, after, rq, rm;

    /* Sleep 10ms relative */
    rq.tv_sec = 0; rq.tv_nsec = 10000000; /* 10ms */
    rm.tv_sec = 0; rm.tv_nsec = 0;

    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    long r = sc4(SYS_CLOCK_NANOSLEEP, CLOCK_MONOTONIC, 0, (long)&rq, (long)&rm);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);

    check_val("nanosleep return", r, 0);

    long elapsed_ns = (after.tv_sec - before.tv_sec) * 1000000000L +
                      (after.tv_nsec - before.tv_nsec);
    check("elapsed >= 10ms", elapsed_ns >= 10000000);
}

/* ── clock_nanosleep03: CLOCK_MONOTONIC + CLOCK_REALTIME relative ── */

static void test_clock_nanosleep03(void) {
    puts("\n[ltp/clock_nanosleep03]\n");

    struct k_timespec before, after, rq, rm;

    /* CLOCK_REALTIME relative 10ms */
    rq.tv_sec = 0; rq.tv_nsec = 10000000;
    rm.tv_sec = 0; rm.tv_nsec = 0;

    sc2(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (long)&before);
    long r = sc4(SYS_CLOCK_NANOSLEEP, CLOCK_REALTIME, 0, (long)&rq, (long)&rm);
    sc2(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (long)&after);
    check_val("realtime sleep ret", r, 0);

    long elapsed_ns = (after.tv_sec - before.tv_sec) * 1000000000L +
                      (after.tv_nsec - before.tv_nsec);
    check("realtime elapsed >= 10ms", elapsed_ns >= 10000000);

    /* CLOCK_MONOTONIC relative 10ms */
    rq.tv_sec = 0; rq.tv_nsec = 10000000;
    rm.tv_sec = 0; rm.tv_nsec = 0;

    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    r = sc4(SYS_CLOCK_NANOSLEEP, CLOCK_MONOTONIC, 0, (long)&rq, (long)&rm);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    check_val("monotonic sleep ret", r, 0);

    elapsed_ns = (after.tv_sec - before.tv_sec) * 1000000000L +
                 (after.tv_nsec - before.tv_nsec);
    check("monotonic elapsed >= 10ms", elapsed_ns >= 10000000);
}

/* ── clock_nanosleep04: TIMER_ABSTIME — absolute sleep ── */

static void test_clock_nanosleep04(void) {
    puts("\n[ltp/clock_nanosleep04]\n");

    struct k_timespec now, target, after;

    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&now);

    /* Absolute target = now + 10ms */
    target.tv_sec = now.tv_sec;
    target.tv_nsec = now.tv_nsec + 10000000;
    if (target.tv_nsec >= 1000000000) {
        target.tv_sec++;
        target.tv_nsec -= 1000000000;
    }

    long r = sc4(SYS_CLOCK_NANOSLEEP, CLOCK_MONOTONIC, TIMER_ABSTIME,
                 (long)&target, 0);
    check_val("abstime sleep ret", r, 0);

    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);

    long elapsed_ns = (after.tv_sec - now.tv_sec) * 1000000000L +
                      (after.tv_nsec - now.tv_nsec);
    check("abstime elapsed >= 10ms", elapsed_ns >= 10000000);
}

TEST("ltp/clock_nanosleep01", test_clock_nanosleep01);
TEST("ltp/clock_nanosleep02", test_clock_nanosleep02);
TEST("ltp/clock_nanosleep03", test_clock_nanosleep03);
TEST("ltp/clock_nanosleep04", test_clock_nanosleep04);
