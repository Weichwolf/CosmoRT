/* LTP clock_gettime01-04 — ported to ktest */
#include "ktest.h"

/* ── clock_gettime01: basic test on multiple clocks ── */

static void test_clock_one(const char *label, int clk) {
    struct k_timespec ts = {0, 0};
    long r = sc2(SYS_CLOCK_GETTIME, clk, (long)&ts);
    if (r == -EINVAL) {
        /* unsupported clock — acceptable */
        pass(label);
        return;
    }
    check_val(label, r, 0);
    if (r != 0) return;
    check(label, ts.tv_sec > 0 || ts.tv_nsec > 0);
    check(label, ts.tv_nsec >= 0 && ts.tv_nsec <= 999999999);
}

static void test_clock_gettime01(void) {
    puts("\n[ltp/clock_gettime01]\n");
    test_clock_one("CLOCK_REALTIME",         CLOCK_REALTIME);
    test_clock_one("CLOCK_MONOTONIC",        CLOCK_MONOTONIC);
    test_clock_one("CLOCK_MONOTONIC_RAW",    CLOCK_MONOTONIC_RAW);
    test_clock_one("CLOCK_REALTIME_COARSE",  CLOCK_REALTIME_COARSE);
    test_clock_one("CLOCK_MONOTONIC_COARSE", CLOCK_MONOTONIC_COARSE);
    test_clock_one("CLOCK_BOOTTIME",         CLOCK_BOOTTIME);
}

/* ── clock_gettime02: EINVAL for invalid clock id ── */

static void test_clock_gettime02(void) {
    puts("\n[ltp/clock_gettime02]\n");

    struct k_timespec ts;
    long r = sc2(SYS_CLOCK_GETTIME, -1, (long)&ts);
    check_val("invalid clock -1 EINVAL", r, -EINVAL);

    r = sc2(SYS_CLOCK_GETTIME, 999, (long)&ts);
    check_val("invalid clock 999 EINVAL", r, -EINVAL);
}

/* ── clock_gettime03: monotonic never goes backwards ── */

static void test_clock_gettime03(void) {
    puts("\n[ltp/clock_gettime03]\n");

    struct k_timespec t1, t2;
    long r1 = sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
    check_val("gettime 1", r1, 0);

    /* Burn some cycles */
    for (volatile int i = 0; i < 10000; i++) {}

    long r2 = sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t2);
    check_val("gettime 2", r2, 0);

    if (r1 == 0 && r2 == 0) {
        long diff_sec = t2.tv_sec - t1.tv_sec;
        long diff_ns = t2.tv_nsec - t1.tv_nsec;
        if (diff_sec < 0 || (diff_sec == 0 && diff_ns < 0))
            fail("monotonic forward", "time went backwards");
        else
            pass("monotonic forward");
    }
}

/* ── clock_gettime04: successive readings within 5ms ── */

static void test_clock_gettime04(void) {
    puts("\n[ltp/clock_gettime04]\n");

    struct k_timespec t1, t2;
    long r1 = sc2(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (long)&t1);
    long r2 = sc2(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (long)&t2);
    check_val("gettime a", r1, 0);
    check_val("gettime b", r2, 0);

    if (r1 == 0 && r2 == 0) {
        long diff_ns = (t2.tv_sec - t1.tv_sec) * 1000000000L + (t2.tv_nsec - t1.tv_nsec);
        /* Must not go backwards */
        check("realtime forward", diff_ns >= 0);
        /* Within 5ms */
        check("realtime delta < 5ms", diff_ns < 5000000);
    }
}

/* ── clock_gettime05: CLOCK_PROCESS_CPUTIME_ID advances monotonically ── */

#ifndef CLOCK_PROCESS_CPUTIME_ID
#define CLOCK_PROCESS_CPUTIME_ID 2
#endif
#ifndef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID 3
#endif

static void test_clock_gettime05_cputime(void) {
    puts("\n[ltp/clock_gettime05-cputime]\n");

    struct k_timespec t1, t2;
    long r1 = sc2(SYS_CLOCK_GETTIME, CLOCK_PROCESS_CPUTIME_ID, (long)&t1);
    check_val("proc_cputime 1", r1, 0);
    for (volatile int i = 0; i < 100000; i++) {}
    long r2 = sc2(SYS_CLOCK_GETTIME, CLOCK_PROCESS_CPUTIME_ID, (long)&t2);
    check_val("proc_cputime 2", r2, 0);
    if (r1 == 0 && r2 == 0) {
        long diff = (t2.tv_sec - t1.tv_sec) * 1000000000L + (t2.tv_nsec - t1.tv_nsec);
        check("proc_cputime forward", diff >= 0);
    }

    struct k_timespec t3;
    long r3 = sc2(SYS_CLOCK_GETTIME, CLOCK_THREAD_CPUTIME_ID, (long)&t3);
    check_val("thread_cputime ok", r3, 0);
    check("thread_cputime nsec valid", t3.tv_nsec >= 0 && t3.tv_nsec < 1000000000L);
}

/* ── clock_gettime06: EFAULT for bad pointer on all valid clocks ── */

static void test_clock_gettime06_efault(void) {
    puts("\n[ltp/clock_gettime06-efault]\n");

    int clocks[] = { CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_PROCESS_CPUTIME_ID,
                     CLOCK_THREAD_CPUTIME_ID, CLOCK_REALTIME_COARSE,
                     CLOCK_MONOTONIC_COARSE, CLOCK_MONOTONIC_RAW, CLOCK_BOOTTIME };
    for (unsigned i = 0; i < sizeof(clocks)/sizeof(clocks[0]); i++) {
        long r = sc2(SYS_CLOCK_GETTIME, clocks[i], 0);
        check_val("NULL ptr EFAULT", r, -EFAULT);
    }
}

TEST("ltp/clock_gettime01",         test_clock_gettime01);
TEST("ltp/clock_gettime02",         test_clock_gettime02);
TEST("ltp/clock_gettime03",         test_clock_gettime03);
TEST("ltp/clock_gettime04",         test_clock_gettime04);
TEST("ltp/clock_gettime05-cputime", test_clock_gettime05_cputime);
TEST("ltp/clock_gettime06-efault",  test_clock_gettime06_efault);
