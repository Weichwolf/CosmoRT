/* LTP clock_getres01 — ported to ktest */
#include "ktest.h"

static void test_getres_one(const char *label, int clk, int expect_ok) {
    struct k_timespec res = {-1, -1};
    long r = sc2(SYS_CLOCK_GETRES, clk, (long)&res);
    if (!expect_ok) {
        check_val(label, r, -EINVAL);
        return;
    }
    if (r == -EINVAL) {
        /* unsupported — acceptable for optional clocks */
        pass(label);
        return;
    }
    check_val(label, r, 0);
    if (r != 0) return;
    check(label, res.tv_sec >= 0);
    check(label, res.tv_nsec >= 0 && res.tv_nsec <= 999999999);
}

/* ── clock_getres01: basic resolution query on all clocks + error ── */

static void test_clock_getres01(void) {
    puts("\n[ltp/clock_getres01]\n");
    test_getres_one("CLOCK_REALTIME",         CLOCK_REALTIME, 1);
    test_getres_one("CLOCK_MONOTONIC",        CLOCK_MONOTONIC, 1);
    test_getres_one("CLOCK_MONOTONIC_RAW",    CLOCK_MONOTONIC_RAW, 1);
    test_getres_one("CLOCK_REALTIME_COARSE",  CLOCK_REALTIME_COARSE, 1);
    test_getres_one("CLOCK_MONOTONIC_COARSE", CLOCK_MONOTONIC_COARSE, 1);
    test_getres_one("CLOCK_BOOTTIME",         CLOCK_BOOTTIME, 1);
    test_getres_one("invalid clock -1",       -1, 0);
}

/* ── clock_getres01 NULL res pointer (should succeed) ── */

static void test_clock_getres01_null(void) {
    puts("\n[ltp/clock_getres01-null]\n");
    long r = sc2(SYS_CLOCK_GETRES, CLOCK_REALTIME, 0);
    check_val("getres NULL res", r, 0);
}

TEST("ltp/clock_getres01",      test_clock_getres01);
TEST("ltp/clock_getres01-null", test_clock_getres01_null);
