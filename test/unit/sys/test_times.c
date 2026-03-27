#include "ktest.h"

/* ── times ───────────────────────────────────── */

struct test_tms {
    long tms_utime, tms_stime, tms_cutime, tms_cstime;
};

static void test_times(void) {
    puts("\n[times]\n");

    struct test_tms tms;
    long r = sc1(SYS_TIMES, (long)&tms);
    check("times returns ticks > 0", r > 0);
    check("tms_utime > 0", tms.tms_utime > 0);
    check_val("tms_stime = 0", tms.tms_stime, 0);
    check_val("tms_cutime = 0", tms.tms_cutime, 0);
    check_val("tms_cstime = 0", tms.tms_cstime, 0);
    /* Return value and tms_utime should be close */
    long diff = r - tms.tms_utime;
    if (diff < 0) diff = -diff;
    check("tms_utime close to return", diff < 10);
}

TEST("times", test_times);
