/* Phase 12: hrtimer_now_ns Hot-Path-Genauigkeit + ns-praezise Sleeps. */
#include "ktest.h"

struct ts_t { long sec; long nsec; };

static long delta_ns(struct ts_t a, struct ts_t b) {
    return (b.sec - a.sec) * 1000000000L + (b.nsec - a.nsec);
}

static void test_hrtimer_monotonic(void) {
    puts("\n[hrtimer ns]\n");

    struct ts_t prev = {0, 0}, cur;
    long monotonic_violations = 0;
    long zero_diffs = 0;
    for (int i = 0; i < 200; i++) {
        long r = sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&cur);
        if (r) { fail("clock_gettime returned error", 0); return; }
        if (i > 0) {
            long d = delta_ns(prev, cur);
            if (d < 0) monotonic_violations++;
            if (d == 0) zero_diffs++;
        }
        prev = cur;
    }
    check_val("CLOCK_MONOTONIC strictly monotonic over 200 reads",
              monotonic_violations, 0);
    check("CLOCK_MONOTONIC sub-ms resolution (<half identical)",
          zero_diffs < 100);
}

static void test_short_nanosleep_precision(void) {
    struct ts_t before, after;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    struct ts_t req = { .sec = 0, .nsec = 500000 };
    long r = sc2(SYS_NANOSLEEP, (long)&req, 0);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    check_val("nanosleep(500us) returns 0", r, 0);
    long d = delta_ns(before, after);
    check("nanosleep(500us) >= 400us", d >= 400000);
    check("nanosleep(500us) <= 5ms",   d <= 5000000);
    puts("  500us slept "); put_int(d / 1000); puts("us\n");
}

static void test_repeated_sub_ms_sleeps(void) {
    struct ts_t before, after;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    for (int i = 0; i < 100; i++) {
        struct ts_t req = { .sec = 0, .nsec = 100000 };
        sc2(SYS_NANOSLEEP, (long)&req, 0);
    }
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    long d = delta_ns(before, after);
    check("100x nanosleep(100us) total >= 10ms", d >= 10000000);
    /* Tickless-LAPIC: jeder Sleep ~150us echter Wake-Latenz auf QEMU-Last. */
    check("100x nanosleep(100us) total <= 50ms (tickless)",
          d <= 50000000);
    puts("  100x100us total "); put_int(d / 1000000); puts("ms\n");
}

static void test_clock_gettime_diff_bounded(void) {
    struct ts_t prev, cur;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&prev);
    long max_diff = 0;
    for (int i = 0; i < 1000; i++) {
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&cur);
        long d = delta_ns(prev, cur);
        if (d > max_diff) max_diff = d;
        prev = cur;
    }
    check("max(diff(clock_gettime)) < 5ms over 1000 calls",
          max_diff < 5000000);
    puts("  max-diff "); put_int(max_diff / 1000); puts("us\n");
}

static void test_clock_gettime_non_zero(void) {
    struct ts_t ts;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&ts);
    check("CLOCK_MONOTONIC > 0 after boot", ts.sec > 0 || ts.nsec > 0);
}

static void test_nanosleep_signal_short(void) {
    struct ts_t req = { .sec = 0, .nsec = 1 };
    long r = sc2(SYS_NANOSLEEP, (long)&req, 0);
    check_val("nanosleep(1ns) returns 0 (rounded up)", r, 0);
}

TEST("hrtimer/monotonic", test_hrtimer_monotonic);
TEST("hrtimer/short_nanosleep_precision", test_short_nanosleep_precision);
TEST("hrtimer/repeated_sub_ms_sleeps", test_repeated_sub_ms_sleeps);
TEST("hrtimer/clock_gettime_diff_bounded", test_clock_gettime_diff_bounded);
TEST("hrtimer/clock_gettime_non_zero", test_clock_gettime_non_zero);
TEST("hrtimer/nanosleep_signal_short", test_nanosleep_signal_short);
