#include "ktest.h"

static void test_time(void) {
    puts("\n[Timers]\n");

    struct { long sec; long nsec; } ts;
    long r = sc2(SYS_clock_gettime, CLOCK_MONOTONIC, (long)&ts);
    check_val("clock_gettime returns 0", r, 0);
    check_ge("time.sec >= 0", ts.sec, 0);
    check("time.nsec in range", ts.nsec >= 0 && ts.nsec < 1000000000);

    puts("  uptime: "); put_int(ts.sec); puts("s ");
    put_int(ts.nsec / 1000000); puts("ms\n");
}

TEST("time", test_time);
