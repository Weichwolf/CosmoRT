#include "ktest.h"

static void test_timer(void) {
    puts("\n[Timer]\n");

    /* ── clock_gettime MONOTONIC: basic sanity ── */
    struct k_timespec t1, t2;
    long r = sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
    check_val("clock_gettime mono ok", r, 0);
    check("mono sec >= 0", t1.tv_sec >= 0);
    check("mono nsec in range", t1.tv_nsec >= 0 && t1.tv_nsec < 1000000000L);

    /* ── monotonicity: two reads, second >= first ── */
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t2);
    long ns1 = t1.tv_sec * 1000000000L + t1.tv_nsec;
    long ns2 = t2.tv_sec * 1000000000L + t2.tv_nsec;
    check("monotonic: t2 >= t1", ns2 >= ns1);

    /* ── nanosleep 1ms: minimum delay ── */
    struct k_timespec sleep_ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1ms */
    struct k_timespec before, after;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    r = sc2(SYS_NANOSLEEP, (long)&sleep_ts, 0);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    check_val("nanosleep 1ms returns 0", r, 0);
    long elapsed_ns = (after.tv_sec - before.tv_sec) * 1000000000L
                    + (after.tv_nsec - before.tv_nsec);
    check_ge("nanosleep 1ms: elapsed >= 900us", elapsed_ns, 900000L);
    check("nanosleep 1ms: elapsed < 500ms", elapsed_ns < 500000000L);

    /* ── nanosleep 10ms: LAPIC one-shot path ── */
    sleep_ts.tv_nsec = 10000000; /* 10ms */
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    sc2(SYS_NANOSLEEP, (long)&sleep_ts, 0);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    long elapsed_ms = (after.tv_sec - before.tv_sec) * 1000
                    + (after.tv_nsec - before.tv_nsec) / 1000000;
    check_ge("nanosleep 10ms: elapsed >= 8ms", elapsed_ms, 8);
    check("nanosleep 10ms: elapsed < 100ms", elapsed_ms < 100);

    /* ── nanosleep 50ms: longer sleep ── */
    sleep_ts.tv_nsec = 50000000; /* 50ms */
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    sc2(SYS_NANOSLEEP, (long)&sleep_ts, 0);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    elapsed_ms = (after.tv_sec - before.tv_sec) * 1000
               + (after.tv_nsec - before.tv_nsec) / 1000000;
    check_ge("nanosleep 50ms: elapsed >= 40ms", elapsed_ms, 40);
    check("nanosleep 50ms: elapsed < 200ms", elapsed_ms < 200);

    /* ── nanosleep 0: immediate return ── */
    sleep_ts.tv_nsec = 0;
    sleep_ts.tv_sec = 0;
    r = sc2(SYS_NANOSLEEP, (long)&sleep_ts, 0);
    check_val("nanosleep 0ns returns 0", r, 0);

    /* ── back-to-back sleeps: timer restored correctly ── */
    sleep_ts.tv_nsec = 2000000; /* 2ms */
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    for (int i = 0; i < 5; i++)
        sc2(SYS_NANOSLEEP, (long)&sleep_ts, 0);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    elapsed_ms = (after.tv_sec - before.tv_sec) * 1000
               + (after.tv_nsec - before.tv_nsec) / 1000000;
    check_ge("5x nanosleep 2ms: elapsed >= 8ms", elapsed_ms, 8);
    check("5x nanosleep 2ms: elapsed < 200ms", elapsed_ms < 200);

    /* ── monotonicity under sleep: clock advances ── */
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
    sleep_ts.tv_nsec = 5000000; /* 5ms */
    sc2(SYS_NANOSLEEP, (long)&sleep_ts, 0);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t2);
    ns1 = t1.tv_sec * 1000000000L + t1.tv_nsec;
    ns2 = t2.tv_sec * 1000000000L + t2.tv_nsec;
    check("clock advances after sleep", ns2 > ns1);
    check_ge("clock advance >= 4ms", ns2 - ns1, 4000000L);

    /* ── concurrent sleeps via fork: LAPIC restore ── */
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* child: sleep 5ms, exit 0 */
        struct k_timespec cts = { .tv_sec = 0, .tv_nsec = 5000000 };
        sc2(SYS_NANOSLEEP, (long)&cts, 0);
        sc1(SYS_EXIT, 0);
    }
    check("fork ok", pid > 0);

    /* parent: sleep 5ms concurrently */
    sleep_ts.tv_nsec = 5000000;
    sc2(SYS_NANOSLEEP, (long)&sleep_ts, 0);

    int wstatus = 0;
    long wp = sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check_val("wait4 returns child", wp, pid);
    check("child exited 0", (wstatus & 0xFF) == 0 && ((wstatus >> 8) & 0xFF) == 0);
}

TEST("timer", test_timer);
