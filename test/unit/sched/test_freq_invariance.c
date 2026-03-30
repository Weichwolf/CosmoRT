/* RT-7: CPU frequency invariance — timer precision + monotonicity */

#include "ktest.h"

#define NS_PER_MS  1000000L
#define NS_PER_SEC 1000000000L
#define EPOCH_2020 1577836800L

static long ts_to_ns(struct k_timespec *t) {
    return t->tv_sec * NS_PER_SEC + t->tv_nsec;
}

static void test_freq_invariance(void) {
    puts("\n[FreqInvariance]\n");

    struct k_timespec t1, t2, before, after, slp;
    long r, ns1, ns2, elapsed_ns, elapsed_ms;

    r = sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
    check_val("fi_mono_read1", r, 0);
    r = sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t2);
    check_val("fi_mono_read2", r, 0);
    check("fi_mono_t2_gt_t1", ts_to_ns(&t2) >= ts_to_ns(&t1));

    slp = (struct k_timespec){ .tv_sec = 0, .tv_nsec = 10 * NS_PER_MS };
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    sc2(SYS_NANOSLEEP, (long)&slp, 0);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    elapsed_ms = (ts_to_ns(&after) - ts_to_ns(&before)) / NS_PER_MS;
    check_ge("fi_sleep10ms_ge8", elapsed_ms, 8);
    check("fi_sleep10ms_lt50", elapsed_ms < 50);

    slp.tv_nsec = 50 * NS_PER_MS;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    sc2(SYS_NANOSLEEP, (long)&slp, 0);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    elapsed_ms = (ts_to_ns(&after) - ts_to_ns(&before)) / NS_PER_MS;
    check_ge("fi_sleep50ms_ge40", elapsed_ms, 40);
    check("fi_sleep50ms_lt150", elapsed_ms < 150);

    slp.tv_nsec = 1 * NS_PER_MS;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    sc2(SYS_NANOSLEEP, (long)&slp, 0);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    elapsed_ns = ts_to_ns(&after) - ts_to_ns(&before);
    check_ge("fi_sleep1ms_ge900us", elapsed_ns, 900000L);

    slp.tv_nsec = 2 * NS_PER_MS;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    for (int i = 0; i < 5; i++)
        sc2(SYS_NANOSLEEP, (long)&slp, 0);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    elapsed_ms = (ts_to_ns(&after) - ts_to_ns(&before)) / NS_PER_MS;
    check_ge("fi_5x2ms_ge8", elapsed_ms, 8);

    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
    slp.tv_nsec = 5 * NS_PER_MS;
    sc2(SYS_NANOSLEEP, (long)&slp, 0);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t2);
    check("fi_advance_after_sleep", ts_to_ns(&t2) > ts_to_ns(&t1));

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct k_timespec ct1, ct2;
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&ct1);
        struct k_timespec cs = { .tv_sec = 0, .tv_nsec = 5 * NS_PER_MS };
        sc2(SYS_NANOSLEEP, (long)&cs, 0);
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&ct2);
        sc1(SYS_EXIT, ts_to_ns(&ct2) > ts_to_ns(&ct1) ? 0 : 1);
    }
    check("fi_fork_ok", pid > 0);
    slp.tv_nsec = 5 * NS_PER_MS;
    sc2(SYS_NANOSLEEP, (long)&slp, 0);
    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("fi_child_mono", (wstatus & 0xFF) == 0 && ((wstatus >> 8) & 0xFF) == 0);

    struct k_timespec prev;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&prev);
    int all_mono = 1;
    for (int i = 0; i < 100; i++) {
        struct k_timespec cur;
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&cur);
        if (ts_to_ns(&cur) < ts_to_ns(&prev)) { all_mono = 0; break; }
        prev = cur;
    }
    check("fi_100reads_monotonic", all_mono);

    sc2(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (long)&t1);
    check("fi_realtime_gt2020", t1.tv_sec > EPOCH_2020);

    int all_ok = 1;
    for (int c = 0; c < 4; c++) {
        pid = sc0(SYS_FORK);
        if (pid == 0) {
            struct k_timespec cs = { .tv_sec = 0, .tv_nsec = 10 * NS_PER_MS };
            struct k_timespec cb, ca;
            sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&cb);
            sc2(SYS_NANOSLEEP, (long)&cs, 0);
            sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&ca);
            long el = (ts_to_ns(&ca) - ts_to_ns(&cb)) / NS_PER_MS;
            sc1(SYS_EXIT, (el >= 8 && el < 100) ? 0 : 1);
        }
    }
    for (int c = 0; c < 4; c++) {
        wstatus = 0;
        sc4(SYS_WAIT4, -1, (long)&wstatus, 0, 0);
        if ((wstatus & 0xFF) != 0 || ((wstatus >> 8) & 0xFF) != 0)
            all_ok = 0;
    }
    check("fi_4children_concurrent_timers", all_ok);
}

TEST("freq_invariance", test_freq_invariance);
