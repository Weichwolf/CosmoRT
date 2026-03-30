/* CosmoRT — RT validation: latency measurement + bounded WCET assertions */
#include "ktest.h"

#define SCHED_OTHER  0
#define SCHED_FIFO   1

#define NSEC_PER_USEC    1000L
#define NSEC_PER_MSEC    1000000L
#define NSEC_PER_SEC     1000000000L
#define USEC_PER_MSEC    1000L

#define WAKE_LATENCY_MAX_US        5000
#define RT_PREEMPT_MAX_US          5000
#define SIGNAL_DELIVERY_MAX_US     5000
#define CTX_SWITCH_AVG_MAX_US      2000
#define NANOSLEEP_JITTER_MAX_US    10000
#define NANOSLEEP_OVERSHOOT_MAX_US 50000
#define TIMER_FAIRNESS_SPREAD_US   20000
#define FUTEX_UNCONTENDED_MAX_US   500000
#define FUTEX_CONTENDED_MAX_US     2000000
#define PIPE_ROUNDTRIP_MAX_US      5000
#define EVENTFD_ROUNDTRIP_MAX_US   5000
#define FORK_LATENCY_MAX_US        50000
#define PREEMPT_GAP_MAX_US         10000
#define YIELD_AVG_MAX_US           1000

#define ROUNDTRIP_COUNT      100
#define NANOSLEEP_ITER       100
#define FUTEX_UNCONTENDED_N  1000
#define FUTEX_CONTENDED_N    100
#define YIELD_ITER           1000
#define PREEMPT_SAMPLES      100

#define NANOSLEEP_1MS_NS     (1 * NSEC_PER_MSEC)
#define NANOSLEEP_10MS_NS    (10 * NSEC_PER_MSEC)
#define CHILD_SETTLE_MS      2
#define TIMER_FAIRNESS_MS    10

#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_LOCK_PI       6
#define FUTEX_UNLOCK_PI     7
#define FUTEX_PRIVATE_FLAG  128

#define PAGE_SIZE 4096

struct sched_param { int sched_priority; };

struct ksigaction_rv {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

__attribute__((naked)) static void sig_restorer_rv(void) {
    __asm__ volatile("mov $15, %%rax\n" "syscall\n" ::: "memory");
}

static long now_ns(void) {
    struct k_timespec ts;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&ts);
    return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

static long now_us(void) {
    return now_ns() / NSEC_PER_USEC;
}

static void msleep(long ms) {
    struct k_timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * NSEC_PER_MSEC };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
}

static volatile int *shared_page(void) {
    return (volatile int *)(long)sc6(SYS_MMAP, 0, PAGE_SIZE,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
}

static void check_lt_us(const char *name, long got_us, long max_us) {
    if (got_us < max_us) {
        pass(name);
    } else {
        puts("  FAIL  "); puts(name);
        puts(" ("); put_int(got_us); puts("us >= "); put_int(max_us); puts("us)\n");
        failures++;
    }
}

/* ── 1. Wake-to-Run Latency ── */

static void test_wake_to_run(void) {
    puts("\n[RT-val]\n");
    int pfd[2];
    sc1(SYS_PIPE2, (long)pfd);

    volatile long *shared = (volatile long *)shared_page();
    if ((long)shared < 0) { fail("wake_to_run", "mmap"); return; }
    shared[0] = 0;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        char buf;
        sc3(SYS_READ, pfd[0], (long)&buf, 1);
        shared[0] = now_us();
        sc1(SYS_EXIT, 0);
    }

    msleep(CHILD_SETTLE_MS);
    long before = now_us();
    char c = 1;
    sc3(SYS_WRITE, pfd[1], (long)&c, 1);

    int ws;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);

    long latency = shared[0] - before;
    puts("  wake-to-run: "); put_int(latency); puts("us\n");
    check_lt_us("wake-to-run < bound", latency, WAKE_LATENCY_MAX_US);

    sc1(SYS_CLOSE, pfd[0]);
    sc1(SYS_CLOSE, pfd[1]);
    sc2(SYS_MUNMAP, (long)shared, PAGE_SIZE);
}

/* ── 2. RT preempts Normal ── */

static void test_rt_preempts_normal(void) {
    volatile long *shared = (volatile long *)shared_page();
    if ((long)shared < 0) { fail("rt_preempts", "mmap"); return; }
    shared[0] = 0;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct sched_param p = { .sched_priority = 20 };
        sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_FIFO, (long)&p);
        struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 5 * NSEC_PER_MSEC };
        sc2(SYS_NANOSLEEP, (long)&ts, 0);
        shared[0] = now_us();
        sc1(SYS_EXIT, 0);
    }

    long deadline = now_us() + 100000;
    while (now_us() < deadline && shared[0] == 0) {
        /* busy-loop as SCHED_OTHER */
    }

    int ws;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);

    long target = 5 * USEC_PER_MSEC;
    long actual = shared[0] ? (long)(shared[0] - (deadline - 100000)) : 99999;
    long overshoot = actual > target ? actual - target : 0;
    puts("  rt-preempt overshoot: "); put_int(overshoot); puts("us\n");
    check_lt_us("rt preempts normal < bound", overshoot, RT_PREEMPT_MAX_US);

    sc2(SYS_MUNMAP, (long)shared, PAGE_SIZE);
}

/* ── 3. Signal-Delivery Latency ── */

static volatile long sig_delivery_ts;

static void sigalrm_handler(int sig) {
    (void)sig;
    sig_delivery_ts = now_us();
}

static void test_signal_delivery(void) {
    sig_delivery_ts = 0;

    struct ksigaction_rv sa = {
        .handler  = (void *)sigalrm_handler,
        .flags    = SA_RESTORER,
        .restorer = (void *)sig_restorer_rv,
        .mask     = 0,
    };
    sc4(SYS_RT_SIGACTION, SIGALRM, (long)&sa, 0, 8);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct ksigaction_rv sa2 = {
            .handler  = (void *)sigalrm_handler,
            .flags    = SA_RESTORER,
            .restorer = (void *)sig_restorer_rv,
            .mask     = 0,
        };
        sc4(SYS_RT_SIGACTION, SIGALRM, (long)&sa2, 0, 8);

        long before = now_us();
        sc1(SYS_ALARM, 1);
        struct k_timespec ts = { .tv_sec = 2, .tv_nsec = 0 };
        sc2(SYS_NANOSLEEP, (long)&ts, 0);
        long latency = sig_delivery_ts > 0 ? sig_delivery_ts - before : 99999999;
        long expected_us = 1 * 1000000L;
        long delta = latency > expected_us ? latency - expected_us : expected_us - latency;
        puts("  signal-delivery delta: "); put_int(delta); puts("us\n");
        sc1(SYS_EXIT, delta < SIGNAL_DELIVERY_MAX_US ? 0 : 1);
    }

    int ws;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    int code = (ws >> 8) & 0xFF;
    check("signal delivery < bound", code == 0);
}

/* ── 4. Context-Switch Latency ── */

static void test_ctx_switch(void) {
    int p2c[2], c2p[2];
    sc1(SYS_PIPE2, (long)p2c);
    sc1(SYS_PIPE2, (long)c2p);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        char buf;
        for (int i = 0; i < ROUNDTRIP_COUNT; i++) {
            sc3(SYS_READ, p2c[0], (long)&buf, 1);
            sc3(SYS_WRITE, c2p[1], (long)&buf, 1);
        }
        sc1(SYS_EXIT, 0);
    }

    long total = 0, max_rt = 0;
    char c = 0;
    for (int i = 0; i < ROUNDTRIP_COUNT; i++) {
        long t0 = now_us();
        sc3(SYS_WRITE, p2c[1], (long)&c, 1);
        sc3(SYS_READ, c2p[0], (long)&c, 1);
        long dt = now_us() - t0;
        total += dt;
        if (dt > max_rt) max_rt = dt;
    }

    int ws;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);

    long avg = total / ROUNDTRIP_COUNT;
    puts("  ctx-switch avg: "); put_int(avg); puts("us max: "); put_int(max_rt); puts("us\n");
    check_lt_us("ctx-switch avg < bound", avg, CTX_SWITCH_AVG_MAX_US);

    sc1(SYS_CLOSE, p2c[0]); sc1(SYS_CLOSE, p2c[1]);
    sc1(SYS_CLOSE, c2p[0]); sc1(SYS_CLOSE, c2p[1]);
}

/* ── 5. nanosleep Jitter ── */

static void test_nanosleep_jitter(void) {
    long min_d = 999999999, max_d = 0, total = 0;
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = NANOSLEEP_1MS_NS };

    for (int i = 0; i < NANOSLEEP_ITER; i++) {
        long t0 = now_us();
        sc2(SYS_NANOSLEEP, (long)&ts, 0);
        long dt = now_us() - t0;
        total += dt;
        if (dt < min_d) min_d = dt;
        if (dt > max_d) max_d = dt;
    }

    long jitter = max_d - min_d;
    puts("  nanosleep jitter: "); put_int(jitter); puts("us (min=");
    put_int(min_d); puts(" max="); put_int(max_d); puts(")\n");
    check_lt_us("nanosleep jitter < bound", jitter, NANOSLEEP_JITTER_MAX_US);
}

/* ── 6. nanosleep Overshoot ── */

static void test_nanosleep_overshoot(void) {
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = NANOSLEEP_10MS_NS };
    long t0 = now_us();
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
    long elapsed = now_us() - t0;
    long expected = 10 * USEC_PER_MSEC;
    long overshoot = elapsed > expected ? elapsed - expected : 0;
    puts("  nanosleep overshoot: "); put_int(overshoot); puts("us (elapsed=");
    put_int(elapsed); puts("us)\n");
    check_lt_us("nanosleep overshoot < bound", overshoot, NANOSLEEP_OVERSHOOT_MAX_US);
}

/* ── 7. Concurrent Timer Fairness ── */

static void test_timer_fairness(void) {
    volatile long *wakes = (volatile long *)shared_page();
    if ((long)wakes < 0) { fail("timer_fairness", "mmap"); return; }
    for (int i = 0; i < 4; i++) wakes[i] = 0;

    long pids[4];
    long start = now_us();
    for (int i = 0; i < 4; i++) {
        pids[i] = sc0(SYS_FORK);
        if (pids[i] == 0) {
            struct k_timespec ts = { .tv_sec = 0, .tv_nsec = TIMER_FAIRNESS_MS * NSEC_PER_MSEC };
            sc2(SYS_NANOSLEEP, (long)&ts, 0);
            wakes[i] = now_us() - start;
            sc1(SYS_EXIT, 0);
        }
    }

    int ws;
    for (int i = 0; i < 4; i++)
        sc4(SYS_WAIT4, pids[i], (long)&ws, 0, 0);

    long wmin = wakes[0], wmax = wakes[0];
    for (int i = 1; i < 4; i++) {
        if (wakes[i] < wmin) wmin = wakes[i];
        if (wakes[i] > wmax) wmax = wakes[i];
    }
    long spread = wmax - wmin;
    puts("  timer fairness spread: "); put_int(spread); puts("us\n");
    check_lt_us("timer fairness spread < bound", spread, TIMER_FAIRNESS_SPREAD_US);

    sc2(SYS_MUNMAP, (long)wakes, PAGE_SIZE);
}

/* ── 8. Uncontended Futex ── */

static void test_futex_uncontended(void) {
    volatile uint32_t futex_var __attribute__((aligned(4))) = 0;
    long t0 = now_us();
    for (int i = 0; i < FUTEX_UNCONTENDED_N; i++) {
        sc6(SYS_FUTEX, (long)&futex_var, FUTEX_LOCK_PI | FUTEX_PRIVATE_FLAG, 0, 0, 0, 0);
        sc6(SYS_FUTEX, (long)&futex_var, FUTEX_UNLOCK_PI | FUTEX_PRIVATE_FLAG, 0, 0, 0, 0);
    }
    long elapsed = now_us() - t0;
    puts("  futex uncontended 1000x: "); put_int(elapsed); puts("us\n");
    check_lt_us("futex uncontended < bound", elapsed, FUTEX_UNCONTENDED_MAX_US);
}

/* ── 9. Contended Futex ── */

static void test_futex_contended(void) {
    volatile uint32_t *fvar = (volatile uint32_t *)shared_page();
    if ((long)fvar < 0) { fail("futex_contended", "mmap"); return; }
    *fvar = 0;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        for (int i = 0; i < FUTEX_CONTENDED_N; i++) {
            sc6(SYS_FUTEX, (long)fvar, FUTEX_LOCK_PI | FUTEX_PRIVATE_FLAG, 0, 0, 0, 0);
            for (volatile int j = 0; j < 100; j++) {}
            sc6(SYS_FUTEX, (long)fvar, FUTEX_UNLOCK_PI | FUTEX_PRIVATE_FLAG, 0, 0, 0, 0);
        }
        sc1(SYS_EXIT, 0);
    }

    long t0 = now_us();
    for (int i = 0; i < FUTEX_CONTENDED_N; i++) {
        sc6(SYS_FUTEX, (long)fvar, FUTEX_LOCK_PI | FUTEX_PRIVATE_FLAG, 0, 0, 0, 0);
        for (volatile int j = 0; j < 100; j++) {}
        sc6(SYS_FUTEX, (long)fvar, FUTEX_UNLOCK_PI | FUTEX_PRIVATE_FLAG, 0, 0, 0, 0);
    }

    int ws;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    long elapsed = now_us() - t0;
    puts("  futex contended: "); put_int(elapsed); puts("us\n");
    check_lt_us("futex contended < bound", elapsed, FUTEX_CONTENDED_MAX_US);

    sc2(SYS_MUNMAP, (long)fvar, PAGE_SIZE);
}

/* ── 10. Pipe Roundtrip ── */

static void test_pipe_roundtrip(void) {
    int p2c[2], c2p[2];
    sc1(SYS_PIPE2, (long)p2c);
    sc1(SYS_PIPE2, (long)c2p);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        char buf;
        for (int i = 0; i < ROUNDTRIP_COUNT; i++) {
            sc3(SYS_READ, p2c[0], (long)&buf, 1);
            sc3(SYS_WRITE, c2p[1], (long)&buf, 1);
        }
        sc1(SYS_EXIT, 0);
    }

    long total = 0;
    char c = 0;
    for (int i = 0; i < ROUNDTRIP_COUNT; i++) {
        long t0 = now_us();
        sc3(SYS_WRITE, p2c[1], (long)&c, 1);
        sc3(SYS_READ, c2p[0], (long)&c, 1);
        total += now_us() - t0;
    }

    int ws;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);

    long avg = total / ROUNDTRIP_COUNT;
    puts("  pipe roundtrip avg: "); put_int(avg); puts("us\n");
    check_lt_us("pipe roundtrip avg < bound", avg, PIPE_ROUNDTRIP_MAX_US);

    sc1(SYS_CLOSE, p2c[0]); sc1(SYS_CLOSE, p2c[1]);
    sc1(SYS_CLOSE, c2p[0]); sc1(SYS_CLOSE, c2p[1]);
}

/* ── 11. eventfd Roundtrip ── */

static void test_eventfd_roundtrip(void) {
    long efd_p = sc2(SYS_EVENTFD2, 0, 0);
    long efd_c = sc2(SYS_EVENTFD2, 0, 0);
    if (efd_p < 0 || efd_c < 0) { fail("eventfd_roundtrip", "eventfd2"); return; }

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        uint64_t val;
        for (int i = 0; i < ROUNDTRIP_COUNT; i++) {
            sc3(SYS_READ, efd_p, (long)&val, 8);
            val = 1;
            sc3(SYS_WRITE, efd_c, (long)&val, 8);
        }
        sc1(SYS_EXIT, 0);
    }

    long total = 0;
    for (int i = 0; i < ROUNDTRIP_COUNT; i++) {
        uint64_t val = 1;
        long t0 = now_us();
        sc3(SYS_WRITE, efd_p, (long)&val, 8);
        sc3(SYS_READ, efd_c, (long)&val, 8);
        total += now_us() - t0;
    }

    int ws;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);

    long avg = total / ROUNDTRIP_COUNT;
    puts("  eventfd roundtrip avg: "); put_int(avg); puts("us\n");
    check_lt_us("eventfd roundtrip avg < bound", avg, EVENTFD_ROUNDTRIP_MAX_US);

    sc1(SYS_CLOSE, efd_p);
    sc1(SYS_CLOSE, efd_c);
}

/* ── 12. Fork Latency ── */

static void test_fork_latency(void) {
    long total = 0;
    #define FORK_ITER 10
    for (int i = 0; i < FORK_ITER; i++) {
        long t0 = now_us();
        long pid = sc0(SYS_FORK);
        if (pid == 0) sc1(SYS_EXIT, 0);
        long dt = now_us() - t0;
        total += dt;
        int ws;
        sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    }
    long avg = total / FORK_ITER;
    puts("  fork avg: "); put_int(avg); puts("us\n");
    check_lt_us("fork latency avg < bound", avg, FORK_LATENCY_MAX_US);
    #undef FORK_ITER
}

/* ── 13. Preemption under Load ── */

static void test_preemption_under_load(void) {
    volatile long *shared = (volatile long *)shared_page();
    if ((long)shared < 0) { fail("preempt_load", "mmap"); return; }
    shared[0] = 0;

    long busy_pid = sc0(SYS_FORK);
    if (busy_pid == 0) {
        while (!shared[0]) {}
        sc1(SYS_EXIT, 0);
    }

    long rt_pid = sc0(SYS_FORK);
    if (rt_pid == 0) {
        struct sched_param p = { .sched_priority = 15 };
        sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_FIFO, (long)&p);

        long max_gap = 0;
        long prev = now_us();
        for (int i = 0; i < PREEMPT_SAMPLES; i++) {
            sc0(SYS_SCHED_YIELD);
            long cur = now_us();
            long gap = cur - prev;
            if (gap > max_gap) max_gap = gap;
            prev = cur;
        }
        sc1(SYS_EXIT, max_gap < PREEMPT_GAP_MAX_US ? 0 : 1);
    }

    int ws;
    sc4(SYS_WAIT4, rt_pid, (long)&ws, 0, 0);
    shared[0] = 1;
    int ws2;
    sc4(SYS_WAIT4, busy_pid, (long)&ws2, 0, 0);

    int code = (ws >> 8) & 0xFF;
    check("preemption under load < bound", code == 0);

    sc2(SYS_MUNMAP, (long)shared, PAGE_SIZE);
}

/* ── 14. Yield Latency ── */

static void test_yield_latency(void) {
    long total = 0, max_y = 0;
    for (int i = 0; i < YIELD_ITER; i++) {
        long t0 = now_us();
        sc0(SYS_SCHED_YIELD);
        long dt = now_us() - t0;
        total += dt;
        if (dt > max_y) max_y = dt;
    }
    long avg = total / YIELD_ITER;
    puts("  yield avg: "); put_int(avg); puts("us max: "); put_int(max_y); puts("us\n");
    check_lt_us("yield avg < bound", avg, YIELD_AVG_MAX_US);
}

/* ── 15. Bounded WCET Smoke ── */

static void test_bounded_wcet_smoke(void) {
    int pfd[2];
    sc1(SYS_PIPE2, (long)pfd);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        char buf;
        sc3(SYS_READ, pfd[0], (long)&buf, 1);
        sc1(SYS_EXIT, 0);
    }

    long t0 = now_us();
    char c = 1;
    sc3(SYS_WRITE, pfd[1], (long)&c, 1);
    int ws;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    long wake_wait = now_us() - t0;

    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = NANOSLEEP_1MS_NS };
    t0 = now_us();
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
    long sleep_elapsed = now_us() - t0;
    long sleep_overshoot = sleep_elapsed > USEC_PER_MSEC ? sleep_elapsed - USEC_PER_MSEC : 0;

    t0 = now_us();
    sc0(SYS_SCHED_YIELD);
    long yield_lat = now_us() - t0;

    int bounded = (wake_wait < WAKE_LATENCY_MAX_US) &&
                  (sleep_overshoot < NANOSLEEP_OVERSHOOT_MAX_US) &&
                  (yield_lat < YIELD_AVG_MAX_US * 10);

    puts("  wcet smoke: wake="); put_int(wake_wait);
    puts("us sleep_os="); put_int(sleep_overshoot);
    puts("us yield="); put_int(yield_lat); puts("us\n");
    check("bounded WCET smoke", bounded);

    sc1(SYS_CLOSE, pfd[0]);
    sc1(SYS_CLOSE, pfd[1]);
}

TEST("rt_val", test_wake_to_run);
TEST("rt_val", test_rt_preempts_normal);
TEST("rt_val", test_signal_delivery);
TEST("rt_val", test_ctx_switch);
TEST("rt_val", test_nanosleep_jitter);
TEST("rt_val", test_nanosleep_overshoot);
TEST("rt_val", test_timer_fairness);
TEST("rt_val", test_futex_uncontended);
TEST("rt_val", test_futex_contended);
TEST("rt_val", test_pipe_roundtrip);
TEST("rt_val", test_eventfd_roundtrip);
TEST("rt_val", test_fork_latency);
TEST("rt_val", test_preemption_under_load);
TEST("rt_val", test_yield_latency);
TEST("rt_val", test_bounded_wcet_smoke);
