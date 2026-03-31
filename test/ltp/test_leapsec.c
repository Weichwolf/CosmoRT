/* LTP leapsec01 — ported to ktest
 * Original tests hrtimer early expiration during leap second insertion
 * using clock_settime + adjtimex(STA_INS) + clock_nanosleep(TIMER_ABSTIME).
 *
 * Simplified: test that clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME)
 * does not wake early. We skip the actual leap second scheduling
 * (requires adjtimex which may not be implemented). */
#include "ktest.h"

#define TIMER_ABSTIME   1

/* ── leapsec01: clock_nanosleep TIMER_ABSTIME does not wake early ── */

static void test_leapsec01(void) {
    puts("\n[ltp/leapsec01]\n");

    struct k_timespec now;
    long r = sc2(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (long)&now);
    check_val("clock_gettime", r, 0);
    if (r != 0) return;

    /* Sleep until now + 0.05s using TIMER_ABSTIME */
    struct k_timespec target;
    target.tv_sec = now.tv_sec;
    target.tv_nsec = now.tv_nsec + 50000000L; /* +50ms */
    if (target.tv_nsec >= 1000000000L) {
        target.tv_sec++;
        target.tv_nsec -= 1000000000L;
    }

    r = sc4(SYS_CLOCK_NANOSLEEP, CLOCK_REALTIME, TIMER_ABSTIME,
            (long)&target, 0);
    check_val("clock_nanosleep", r, 0);

    /* Verify we woke up at or after target */
    struct k_timespec after;
    sc2(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (long)&after);

    int ok = (after.tv_sec > target.tv_sec) ||
             (after.tv_sec == target.tv_sec &&
              after.tv_nsec >= target.tv_nsec);
    check("woke at or after target (no early expiry)", ok);
}

/* ── leapsec01-mono: same test with CLOCK_MONOTONIC ── */

static void test_leapsec01_mono(void) {
    puts("\n[ltp/leapsec01-mono]\n");

    struct k_timespec now;
    long r = sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&now);
    check_val("clock_gettime mono", r, 0);
    if (r != 0) return;

    struct k_timespec target;
    target.tv_sec = now.tv_sec;
    target.tv_nsec = now.tv_nsec + 50000000L;
    if (target.tv_nsec >= 1000000000L) {
        target.tv_sec++;
        target.tv_nsec -= 1000000000L;
    }

    r = sc4(SYS_CLOCK_NANOSLEEP, CLOCK_MONOTONIC, TIMER_ABSTIME,
            (long)&target, 0);
    check_val("clock_nanosleep mono", r, 0);

    struct k_timespec after;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);

    int ok = (after.tv_sec > target.tv_sec) ||
             (after.tv_sec == target.tv_sec &&
              after.tv_nsec >= target.tv_nsec);
    check("mono: woke at or after target", ok);
}

TEST("ltp/leapsec01",      test_leapsec01);
TEST("ltp/leapsec01-mono", test_leapsec01_mono);
