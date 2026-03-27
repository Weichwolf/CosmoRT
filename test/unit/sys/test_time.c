#include "ktest.h"

static void test_time(void) {
    puts("\n[Timers]\n");

    /* CLOCK_MONOTONIC: uptime, should be small */
    struct { long sec; long nsec; } mono;
    long r = sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&mono);
    check_val("clock_gettime MONOTONIC returns 0", r, 0);
    check_ge("monotonic.sec >= 0", mono.sec, 0);
    check("monotonic.sec < 3600 (uptime)", mono.sec < 3600);
    check("monotonic.nsec in range", mono.nsec >= 0 && mono.nsec < 1000000000);

    puts("  uptime: "); put_int(mono.sec); puts("s ");
    put_int(mono.nsec / 1000000); puts("ms\n");

    /* CLOCK_REALTIME: wall clock, must be post-2023 */
    struct { long sec; long nsec; } real;
    r = sc2(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (long)&real);
    check_val("clock_gettime REALTIME returns 0", r, 0);
    check("realtime.sec > 1700000000", real.sec > 1700000000L);
    check("realtime.nsec in range", real.nsec >= 0 && real.nsec < 1000000000);

    puts("  epoch: "); put_int(real.sec); puts("s\n");

    /* gettimeofday: wall clock */
    struct { long sec; long usec; } tv;
    r = sc2(SYS_GETTIMEOFDAY, (long)&tv, 0);
    check_val("gettimeofday returns 0", r, 0);
    check("gettimeofday.sec > 1700000000", tv.sec > 1700000000L);
    check("gettimeofday.usec in range", tv.usec >= 0 && tv.usec < 1000000);
}

TEST("time", test_time);
