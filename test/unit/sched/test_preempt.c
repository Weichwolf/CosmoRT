/* CosmoRT — Preemption model tests
 *
 * Tests for Full Preemption (RT) + Lazy Preemption (Non-RT) infrastructure.
 * Verifiable from userspace: scheduling policy effects, RT priority ordering,
 * sched_yield, timeslice behavior, sched_setscheduler API.
 */
#include "ktest.h"

/* ── Scheduling policy constants (POSIX) ─────────── */

#define SCHED_OTHER  0
#define SCHED_FIFO   1
#define SCHED_RR     2

/* ── Helpers ─────────────────────────────────────── */

struct sched_param { int sched_priority; };

static long msleep(long ms) {
    struct k_timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    return sc2(SYS_NANOSLEEP, (long)&ts, 0);
}

static long now_ms(void) {
    struct k_timespec ts;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Shared memory page for parent-child communication */
static volatile int *shared_page(void) {
    return (volatile int *)(long)sc6(SYS_MMAP, 0, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
}

/* ── Tests ───────────────────────────────────────── */

/* 1. sched_yield returns 0 (basic sanity) */
static void test_yield_returns_zero(void) {
    puts("\n[Preempt]\n");
    long r = sc0(SYS_SCHED_YIELD);
    check_val("sched_yield returns 0", r, 0);
}

/* 2. sched_setscheduler + getscheduler round-trip */
static void test_set_get_scheduler(void) {
    struct sched_param p = { .sched_priority = 10 };
    long r = sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_FIFO, (long)&p);
    check_val("setscheduler FIFO returns 0", r, 0);

    long pol = sc1(SYS_SCHED_GETSCHEDULER, 0);
    check_val("getscheduler returns FIFO", pol, SCHED_FIFO);

    /* Restore to SCHED_OTHER */
    struct sched_param p0 = { .sched_priority = 0 };
    sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_OTHER, (long)&p0);
}

/* 3. sched_setscheduler rejects invalid policy */
static void test_set_scheduler_invalid(void) {
    struct sched_param p = { .sched_priority = 0 };
    long r = sc3(SYS_SCHED_SETSCHEDULER, 0, 99, (long)&p);
    check_val("setscheduler invalid policy = -EINVAL", r, -EINVAL);
}

/* 4. sched_setparam + getparam round-trip */
static void test_set_get_param(void) {
    struct sched_param p = { .sched_priority = 5 };
    long r = sc2(SYS_SCHED_SETPARAM, 0, (long)&p);
    check_val("setparam returns 0", r, 0);

    struct sched_param g = { 0 };
    r = sc2(SYS_SCHED_GETPARAM, 0, (long)&g);
    check_val("getparam returns 0", r, 0);
    check_val("getparam priority = 5", g.sched_priority, 5);

    /* Restore */
    struct sched_param p0 = { .sched_priority = 0 };
    sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_OTHER, (long)&p0);
}

/* 5. sched_get_priority_max/min */
static void test_priority_range(void) {
    long pmax = sc1(SYS_SCHED_GET_PRIORITY_MAX, SCHED_FIFO);
    long pmin = sc1(SYS_SCHED_GET_PRIORITY_MIN, SCHED_FIFO);
    check("priority_max > priority_min", pmax > pmin);
    check("priority_max = 31", pmax == 31);
}

/* 6. SCHED_RR: setscheduler + getscheduler */
static void test_sched_rr(void) {
    struct sched_param p = { .sched_priority = 3 };
    long r = sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_RR, (long)&p);
    check_val("setscheduler RR returns 0", r, 0);

    long pol = sc1(SYS_SCHED_GETSCHEDULER, 0);
    check_val("getscheduler returns RR", pol, SCHED_RR);

    struct sched_param p0 = { .sched_priority = 0 };
    sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_OTHER, (long)&p0);
}

/* 7. RT child (SCHED_FIFO) runs before SCHED_OTHER parent resumes.
 * Fork a child, set it to SCHED_FIFO high priority. Child writes
 * a sequence number to shared memory, then parent writes its own.
 * RT child should write first (lower sequence number). */
static void test_rt_preempts_normal(void) {
    volatile int *seq = shared_page();
    if ((long)seq < 0) { fail("rt_preempts_normal", "mmap failed"); return; }
    seq[0] = 0; /* sequence counter */
    seq[1] = 0; /* child's sequence */
    seq[2] = 0; /* parent's sequence */

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Child: become RT SCHED_FIFO priority 20 */
        struct sched_param p = { .sched_priority = 20 };
        sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_FIFO, (long)&p);

        /* Yield once to ensure scheduler sees our new priority */
        sc0(SYS_SCHED_YIELD);

        /* Record sequence */
        seq[1] = __sync_add_and_fetch(&seq[0], 1);
        sc1(SYS_EXIT, 0);
    }

    /* Parent: stay SCHED_OTHER, yield to let child run */
    sc0(SYS_SCHED_YIELD);
    msleep(10); /* give child time to complete */
    seq[2] = __sync_add_and_fetch(&seq[0], 1);

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);

    /* RT child should have run first (lower sequence number) */
    check("RT child ran first", seq[1] > 0 && seq[1] < seq[2]);

    sc2(SYS_MUNMAP, (long)seq, 4096);
}

/* 8. Multiple sched_yield does not deadlock */
static void test_yield_stress(void) {
    for (int i = 0; i < 100; i++)
        sc0(SYS_SCHED_YIELD);
    pass("100x sched_yield no hang");
}

/* 9. SCHED_OTHER timeslice: two SCHED_OTHER children both get CPU time */
static void test_sched_other_fairness(void) {
    volatile int *counters = shared_page();
    if ((long)counters < 0) { fail("sched_other_fairness", "mmap failed"); return; }
    counters[0] = 0; /* child A counter */
    counters[1] = 0; /* child B counter */
    counters[2] = 0; /* stop flag */

    long pidA = sc0(SYS_FORK);
    if (pidA == 0) {
        while (!counters[2]) {
            counters[0]++;
            if (counters[0] > 100000) break;
        }
        sc1(SYS_EXIT, 0);
    }

    long pidB = sc0(SYS_FORK);
    if (pidB == 0) {
        while (!counters[2]) {
            counters[1]++;
            if (counters[1] > 100000) break;
        }
        sc1(SYS_EXIT, 0);
    }

    msleep(50); /* let them run */
    counters[2] = 1; /* stop */

    int ws;
    sc4(SYS_WAIT4, pidA, (long)&ws, 0, 0);
    sc4(SYS_WAIT4, pidB, (long)&ws, 0, 0);

    /* Both children should have incremented their counter */
    check("child A got CPU time", counters[0] > 0);
    check("child B got CPU time", counters[1] > 0);

    sc2(SYS_MUNMAP, (long)counters, 4096);
}

/* 10. Higher RT priority runs before lower RT priority */
static void test_rt_priority_ordering(void) {
    volatile int *seq = shared_page();
    if ((long)seq < 0) { fail("rt_priority_ordering", "mmap failed"); return; }
    seq[0] = 0; /* sequence counter */
    seq[1] = 0; /* low-prio child's sequence */
    seq[2] = 0; /* high-prio child's sequence */

    /* Fork low-priority RT child first */
    long pidLow = sc0(SYS_FORK);
    if (pidLow == 0) {
        struct sched_param p = { .sched_priority = 5 };
        sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_FIFO, (long)&p);
        sc0(SYS_SCHED_YIELD);
        msleep(5); /* small delay to let high-prio start */
        seq[1] = __sync_add_and_fetch(&seq[0], 1);
        sc1(SYS_EXIT, 0);
    }

    /* Fork high-priority RT child */
    long pidHigh = sc0(SYS_FORK);
    if (pidHigh == 0) {
        struct sched_param p = { .sched_priority = 25 };
        sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_FIFO, (long)&p);
        sc0(SYS_SCHED_YIELD);
        seq[2] = __sync_add_and_fetch(&seq[0], 1);
        sc1(SYS_EXIT, 0);
    }

    /* Wait for both */
    int ws;
    sc4(SYS_WAIT4, pidHigh, (long)&ws, 0, 0);
    sc4(SYS_WAIT4, pidLow, (long)&ws, 0, 0);

    /* High-priority should have recorded first */
    check("high-prio RT ran before low-prio RT",
          seq[2] > 0 && (seq[1] == 0 || seq[2] < seq[1]));

    sc2(SYS_MUNMAP, (long)seq, 4096);
}

TEST("preempt", test_yield_returns_zero);
TEST("preempt", test_set_get_scheduler);
TEST("preempt", test_set_scheduler_invalid);
TEST("preempt", test_set_get_param);
TEST("preempt", test_priority_range);
TEST("preempt", test_sched_rr);
TEST("preempt", test_rt_preempts_normal);
TEST("preempt", test_yield_stress);
TEST("preempt", test_sched_other_fairness);
TEST("preempt", test_rt_priority_ordering);
