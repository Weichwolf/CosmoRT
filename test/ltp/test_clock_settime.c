/* LTP clock_settime01-03 — ported to ktest */
#include "ktest.h"

/* ── clock_settime01: advance and recede CLOCK_REALTIME ── */

static void test_clock_settime_advance(void) {
    puts("\n[ltp/clock_settime01-advance]\n");

    struct k_timespec before, change, after;

    long r = sc2(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (long)&before);
    check_val("clock_gettime", r, 0);

    /* advance 10 seconds */
    change.tv_sec = before.tv_sec + 10;
    change.tv_nsec = before.tv_nsec;

    r = sc2(SYS_CLOCK_SETTIME, CLOCK_REALTIME, (long)&change);
    check_val("clock_settime advance", r, 0);

    r = sc2(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (long)&after);
    check_val("clock_gettime after", r, 0);

    long elapsed = after.tv_sec - before.tv_sec;
    check_ge("time advanced >= 9s", elapsed, 9);

    /* restore: go back 10 seconds */
    struct k_timespec restore;
    restore.tv_sec = after.tv_sec - 10;
    restore.tv_nsec = after.tv_nsec;
    sc2(SYS_CLOCK_SETTIME, CLOCK_REALTIME, (long)&restore);
}

/* ── clock_settime02: EINVAL for bad timespec ── */

static void test_clock_settime_einval_neg_sec(void) {
    puts("\n[ltp/clock_settime02-neg-sec]\n");

    struct k_timespec ts = { .tv_sec = -1, .tv_nsec = 0 };
    long r = sc2(SYS_CLOCK_SETTIME, CLOCK_REALTIME, (long)&ts);
    check_val("tv_sec=-1 EINVAL", r, -EINVAL);
}

static void test_clock_settime_einval_neg_nsec(void) {
    puts("\n[ltp/clock_settime02-neg-nsec]\n");

    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = -1 };
    long r = sc2(SYS_CLOCK_SETTIME, CLOCK_REALTIME, (long)&ts);
    check_val("tv_nsec=-1 EINVAL", r, -EINVAL);
}

static void test_clock_settime_einval_nsec_overflow(void) {
    puts("\n[ltp/clock_settime02-nsec-overflow]\n");

    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 1000000001L };
    long r = sc2(SYS_CLOCK_SETTIME, CLOCK_REALTIME, (long)&ts);
    check_val("tv_nsec>1e9 EINVAL", r, -EINVAL);
}

/* ── clock_settime02: EINVAL for non-settable clocks ── */

static void test_clock_settime_einval_monotonic(void) {
    puts("\n[ltp/clock_settime02-monotonic]\n");

    struct k_timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
    long r = sc2(SYS_CLOCK_SETTIME, CLOCK_MONOTONIC, (long)&ts);
    check_val("CLOCK_MONOTONIC EINVAL", r, -EINVAL);
}

static void test_clock_settime_einval_boottime(void) {
    puts("\n[ltp/clock_settime02-boottime]\n");

    struct k_timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
    long r = sc2(SYS_CLOCK_SETTIME, CLOCK_BOOTTIME, (long)&ts);
    check_val("CLOCK_BOOTTIME EINVAL", r, -EINVAL);
}

/* ── clock_settime02: EINVAL for invalid clock id ── */

static void test_clock_settime_einval_badclock(void) {
    puts("\n[ltp/clock_settime02-badclock]\n");

    struct k_timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
    long r = sc2(SYS_CLOCK_SETTIME, 99, (long)&ts);
    check_val("invalid clock EINVAL", r, -EINVAL);
}

/* ── clock_settime02: EFAULT for bad pointer ── */

static void test_clock_settime_efault(void) {
    puts("\n[ltp/clock_settime02-efault]\n");

    long r = sc2(SYS_CLOCK_SETTIME, CLOCK_REALTIME, 0);
    check_val("NULL timespec EFAULT", r, -EFAULT);
}

/* ── clock_settime01: recede CLOCK_REALTIME by 10s, then restore ── */

static void test_clock_settime_recede(void) {
    puts("\n[ltp/clock_settime01-recede]\n");

    struct k_timespec before, change, after;

    long r = sc2(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (long)&before);
    check_val("gettime before", r, 0);

    change.tv_sec = before.tv_sec - 10;
    change.tv_nsec = before.tv_nsec;

    r = sc2(SYS_CLOCK_SETTIME, CLOCK_REALTIME, (long)&change);
    check_val("settime recede", r, 0);

    r = sc2(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (long)&after);
    check_val("gettime after", r, 0);

    long elapsed = after.tv_sec - before.tv_sec;
    check("time receded", elapsed <= -9 && elapsed >= -11);

    /* restore forward */
    struct k_timespec restore;
    restore.tv_sec = after.tv_sec + 10;
    restore.tv_nsec = after.tv_nsec;
    sc2(SYS_CLOCK_SETTIME, CLOCK_REALTIME, (long)&restore);
}

TEST("ltp/clock_settime01-recede",         test_clock_settime_recede);
TEST("ltp/clock_settime01-advance",        test_clock_settime_advance);
TEST("ltp/clock_settime02-neg-sec",        test_clock_settime_einval_neg_sec);
TEST("ltp/clock_settime02-neg-nsec",       test_clock_settime_einval_neg_nsec);
TEST("ltp/clock_settime02-nsec-overflow",  test_clock_settime_einval_nsec_overflow);
TEST("ltp/clock_settime02-monotonic",      test_clock_settime_einval_monotonic);
TEST("ltp/clock_settime02-boottime",       test_clock_settime_einval_boottime);
TEST("ltp/clock_settime02-badclock",       test_clock_settime_einval_badclock);
TEST("ltp/clock_settime02-efault",         test_clock_settime_efault);
