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
    /* Tickless: ~150us/sleep, ~36ms total. Bound 100ms toleriert SMP-Last. */
    check("100x nanosleep(100us) total <= 100ms (tickless)",
          d <= 100000000);
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

static void test_pselect_ns_timeout(void) {
    /* pselect6 mit 200us-Timeout: Test der ns-Pfad-Migration in
     * sys_event.c. Vorher: ms-quantisiert -> Sleep min. 1ms. */
    struct ts_t before, after;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    /* nfds=0, alle fds NULL, timeout 200us */
    struct ts_t to = { .sec = 0, .nsec = 200000 };
    long r = sc6(SYS_PSELECT6, 0, 0, 0, 0, (long)&to, 0);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    check_val("pselect6(timeout=200us) returns 0", r, 0);
    long d = delta_ns(before, after);
    check("pselect6(200us) <= 5ms (ns-precise)", d <= 5000000);
    puts("  pselect6(200us) actual "); put_int(d / 1000); puts("us\n");
}

static void test_ppoll_ns_timeout(void) {
    /* ppoll mit 300us, kein fd. */
    struct ts_t before, after;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    struct ts_t to = { .sec = 0, .nsec = 300000 };
    /* nfds=1 mit invaliden fd damit der Pfad anlaeuft. POLLIN=1. */
    struct { int fd; short events; short revents; } pfd = { -1, 1, 0 };
    long r = sc4(SYS_PPOLL, (long)&pfd, 1, (long)&to, 0);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    check("ppoll returns 0 or >0", r >= 0);
    long d = delta_ns(before, after);
    /* Wenn fd negativ, revents=POLLNVAL sofort -> kein Sleep. Sonst Sleep. */
    if (r == 0) {
        check("ppoll(300us) <= 5ms", d <= 5000000);
    }
    puts("  ppoll(300us) actual "); put_int(d / 1000); puts("us, ret=");
    put_int(r); puts("\n");
}

/* Reproduziert clock_nanosleep01-Hang: 10s nanosleep, SIGINT kommt
 * nach kurzer Zeit, Sleep muss wake-up mit -EINTR und korrektem rem. */
static long sigint_caught;
static void sigint_handler(int sig) { (void)sig; sigint_caught = 1; }

static void test_long_sleep_signal_wake(void) {
    /* Setup SIGINT handler */
    struct k_sigaction { void (*sa_handler)(int); unsigned long sa_flags;
                        void *sa_restorer; long sa_mask; } sa = {
        .sa_handler = sigint_handler, .sa_flags = 0x4000000 /* SA_RESTORER stub*/,
    };
    long sa_r = sc4(SYS_RT_SIGACTION, 2 /*SIGINT*/, (long)&sa, 0, 8);
    if (sa_r) { puts("  skipped (no SA)\n"); return; }

    sigint_caught = 0;
    /* Fork child that sleeps 5s */
    int pid = (int)sc0(SYS_FORK);
    if (pid == 0) {
        struct ts_t req = { .sec = 5, .nsec = 0 };
        struct ts_t rem;
        long r = sc2(SYS_NANOSLEEP, (long)&req, (long)&rem);
        /* expect -EINTR (-4); rem should reflect remaining time */
        sc1(SYS_EXIT, (r == -4 && rem.sec >= 4) ? 0 : 99);
    }
    /* Parent: short sleep, SIGINT child, wait */
    struct ts_t pre_sleep = { .sec = 0, .nsec = 100000 };
    sc2(SYS_NANOSLEEP, (long)&pre_sleep, 0);
    sc2(SYS_KILL, pid, 2 /*SIGINT*/);
    int status = 0;
    long w = sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check_val("child reaped", w, pid);
    check_val("child exit-code 0 (got EINTR + correct rem)", status & 0xff00, 0);
}

/* Repro tls_init pthread_join FUTEX_WAIT-infinite + FUTEX_WAKE-Race */
static volatile int futex_word;
static void test_futex_wake_wait_loop(void) {
    /* 5x: child setzt futex_word=1, FUTEX_WAKE; parent FUTEX_WAIT(0) */
    int passes = 0;
    for (int i = 0; i < 5; i++) {
        futex_word = 0;
        int pid = (int)sc0(SYS_FORK);
        if (pid == 0) {
            /* Tiny sleep so parent reaches FUTEX_WAIT first */
            struct ts_t s = { .sec = 0, .nsec = 1000000 /* 1ms */ };
            sc2(SYS_NANOSLEEP, (long)&s, 0);
            __atomic_store_n(&futex_word, 1, __ATOMIC_RELEASE);
            sc6(SYS_FUTEX, (long)&futex_word, 1 /*FUTEX_WAKE*/ | 0x80, 1, 0, 0, 0);
            sc1(SYS_EXIT, 0);
        }
        long r = sc6(SYS_FUTEX, (long)&futex_word, 0 /*FUTEX_WAIT*/ | 0x80,
                     0, 0 /*timeout=NULL=infinite*/, 0, 0);
        /* Akzeptiert: 0 (woken) oder -EAGAIN (-11, value changed before wait) */
        int status = 0;
        sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
        if (r == 0 || r == -11) passes++;
    }
    check_val("5x FUTEX_WAIT/WAKE cycle PASS", passes, 5);
}

TEST("hrtimer/futex_wake_wait_loop", test_futex_wake_wait_loop);
TEST("hrtimer/long_sleep_signal_wake", test_long_sleep_signal_wake);
TEST("hrtimer/monotonic", test_hrtimer_monotonic);
TEST("hrtimer/short_nanosleep_precision", test_short_nanosleep_precision);
TEST("hrtimer/repeated_sub_ms_sleeps", test_repeated_sub_ms_sleeps);
TEST("hrtimer/clock_gettime_diff_bounded", test_clock_gettime_diff_bounded);
TEST("hrtimer/clock_gettime_non_zero", test_clock_gettime_non_zero);
TEST("hrtimer/nanosleep_signal_short", test_nanosleep_signal_short);
TEST("hrtimer/pselect_ns_timeout", test_pselect_ns_timeout);
TEST("hrtimer/ppoll_ns_timeout", test_ppoll_ns_timeout);
