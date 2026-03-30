/* CosmoRT IRQ Thread infrastructure — indirect validation tests */
#include "ktest.h"

#define PIPE_READ  0
#define PIPE_WRITE 1
#define SCHED_OTHER 0
#define SCHED_FIFO  1

static void msleep(int ms) {
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = ms * 1000000L };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
}

static void test_timer_irq_latency(void) {
    puts("\n[irq_thread]\n");
    struct k_timespec req = { .tv_sec = 0, .tv_nsec = 10000000L };
    struct k_timespec rem = {0};
    long r = sc2(SYS_NANOSLEEP, (long)&req, (long)&rem);
    check("nanosleep 10ms returns 0", r == 0 || r == -EINTR);
}

static void test_fork_pipe_roundtrip(void) {
    int pfd[2];
    long r = sc1(SYS_PIPE, (long)pfd);
    check_val("pipe create", r, 0);
    if (r != 0) return;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_CLOSE, pfd[PIPE_READ]);
        char msg = 'K';
        sc3(SYS_WRITE, pfd[PIPE_WRITE], (long)&msg, 1);
        sc1(SYS_CLOSE, pfd[PIPE_WRITE]);
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }
    check("fork succeeded", pid > 0);
    if (pid <= 0) return;

    sc1(SYS_CLOSE, pfd[PIPE_WRITE]);
    char buf = 0;
    long n = sc3(SYS_READ, pfd[PIPE_READ], (long)&buf, 1);
    sc1(SYS_CLOSE, pfd[PIPE_READ]);
    check_val("pipe read 1 byte", n, 1);
    check_val("pipe data correct", (long)buf, 'K');
    sc4(SYS_WAIT4, pid, 0, 0, 0);
}

static void test_nanosleep_back_to_back(void) {
    for (int i = 0; i < 5; i++) {
        struct k_timespec req = { .tv_sec = 0, .tv_nsec = 5000000L };
        long r = sc2(SYS_NANOSLEEP, (long)&req, 0);
        if (r != 0 && r != -EINTR) {
            fail("nanosleep back-to-back", 0);
            return;
        }
    }
    pass("nanosleep 5x5ms stable");
}

static void test_fork_stress(void) {
    long pids[4];
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        pids[i] = sc0(SYS_FORK);
        if (pids[i] == 0) {
            msleep(5);
            sc1(SYS_EXIT, 0);
            __builtin_unreachable();
        }
        if (pids[i] < 0) ok = 0;
    }
    check("fork 4 children", ok);
    for (int i = 0; i < 4; i++) {
        if (pids[i] > 0)
            sc4(SYS_WAIT4, pids[i], 0, 0, 0);
    }
    pass("fork stress 4 children reaped");
}

static void test_epoll_timeout(void) {
    long efd = sc4(SYS_EPOLL_CREATE1, 0, 0, 0, 0);
    check("epoll_create1", efd >= 0);
    if (efd < 0) return;

    struct { uint32_t events; uint64_t data; } __attribute__((packed)) ev;
    long r = sc4(SYS_EPOLL_WAIT, efd, (long)&ev, 1, 10);
    check_val("epoll_wait timeout 10ms = 0", r, 0);
    sc1(SYS_CLOSE, efd);
}

__attribute__((naked)) static void irqt_sig_restorer(void) {
    __asm__ volatile("mov $15, %%rax\n" "syscall\n" ::: "memory");
}

static volatile int irqt_got_alarm;

static void irqt_alarm_handler(int sig) {
    (void)sig;
    irqt_got_alarm = 1;
}

static void test_sigalrm_delivery(void) {
    struct {
        void *handler;
        unsigned long flags;
        void (*restorer)(void);
        uint64_t mask;
    } sa;
    for (int i = 0; i < (int)sizeof(sa); i++) ((char*)&sa)[i] = 0;

    irqt_got_alarm = 0;
    sa.handler = (void *)irqt_alarm_handler;
    sa.flags = 0x04000000;
    sa.restorer = (void *)irqt_sig_restorer;
    sc4(SYS_RT_SIGACTION, SIGALRM, (long)&sa, 0, 8);

    sc1(SYS_ALARM, 1);
    struct k_timespec req = { .tv_sec = 3, .tv_nsec = 0 };
    long r = sc2(SYS_NANOSLEEP, (long)&req, 0);
    check("SIGALRM interrupts sleep", r == -EINTR);
    sc1(SYS_ALARM, 0);
}

static void test_pipe_contention(void) {
    int pfd[2];
    long r = sc1(SYS_PIPE, (long)pfd);
    if (r != 0) { fail("pipe create", 0); return; }

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_CLOSE, pfd[PIPE_READ]);
        char data[64];
        for (int i = 0; i < 64; i++) data[i] = (char)(i & 0xFF);
        sc3(SYS_WRITE, pfd[PIPE_WRITE], (long)data, 64);
        sc1(SYS_CLOSE, pfd[PIPE_WRITE]);
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }
    check("pipe contention fork", pid > 0);
    if (pid <= 0) return;

    sc1(SYS_CLOSE, pfd[PIPE_WRITE]);
    char buf[64];
    long total = 0;
    while (total < 64) {
        long n = sc3(SYS_READ, pfd[PIPE_READ], (long)(buf + total), 64 - total);
        if (n <= 0) break;
        total += n;
    }
    sc1(SYS_CLOSE, pfd[PIPE_READ]);
    check_val("pipe contention read 64B", total, 64);
    sc4(SYS_WAIT4, pid, 0, 0, 0);
}

static void test_sched_yield_stress(void) {
    for (int i = 0; i < 100; i++)
        sc0(SYS_SCHED_YIELD);
    pass("sched_yield 100x");
}

struct sched_param { int sched_priority; };

static void test_rt_priority_ordering(void) {
    struct sched_param p = { .sched_priority = 10 };
    long r = sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_FIFO, (long)&p);
    check("set SCHED_FIFO prio 10", r == 0);

    int policy = (int)sc1(SYS_SCHED_GETSCHEDULER, 0);
    check_val("getscheduler = SCHED_FIFO", (long)policy, SCHED_FIFO);

    struct sched_param p2 = {0};
    sc2(SYS_SCHED_GETPARAM, 0, (long)&p2);
    check_val("getparam prio = 10", (long)p2.sched_priority, 10);

    p.sched_priority = 0;
    sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_OTHER, (long)&p);
}

static void test_futex_wait_wake(void) {
    volatile uint32_t futex_var = 0;
    long r = sc6(SYS_FUTEX, (long)&futex_var, 0 | 128, 1, 0, 0, 0);
    check_val("futex WAIT wrong val = -EAGAIN", r, -EAGAIN);

    r = sc6(SYS_FUTEX, (long)&futex_var, 1 | 128, 1, 0, 0, 0);
    check_val("futex WAKE no waiters = 0", r, 0);
}

static void test_nanosleep_precision(void) {
    struct k_timespec before, after;
    sc2(SYS_CLOCK_GETTIME, 1, (long)&before);

    struct k_timespec req = { .tv_sec = 0, .tv_nsec = 10000000L };
    sc2(SYS_NANOSLEEP, (long)&req, 0);

    sc2(SYS_CLOCK_GETTIME, 1, (long)&after);

    long elapsed_ns = (after.tv_sec - before.tv_sec) * 1000000000L +
                      (after.tv_nsec - before.tv_nsec);
    long elapsed_ms = elapsed_ns / 1000000L;

    check("nanosleep 10ms precision >= 8ms", elapsed_ms >= 8);
    check("nanosleep 10ms precision <= 50ms", elapsed_ms <= 50);
}

static void test_getpid_after_irq(void) {
    long pid1 = sc0(SYS_GETPID);
    msleep(5);
    long pid2 = sc0(SYS_GETPID);
    check("getpid stable across timer IRQs", pid1 == pid2 && pid1 > 0);
}

TEST("irqt_timer_latency",     test_timer_irq_latency);
TEST("irqt_fork_pipe_rt",      test_fork_pipe_roundtrip);
TEST("irqt_nanosleep_b2b",     test_nanosleep_back_to_back);
TEST("irqt_fork_stress",       test_fork_stress);
TEST("irqt_epoll_timeout",     test_epoll_timeout);
TEST("irqt_sigalrm",           test_sigalrm_delivery);
TEST("irqt_pipe_contention",   test_pipe_contention);
TEST("irqt_yield_stress",      test_sched_yield_stress);
TEST("irqt_rt_prio_order",     test_rt_priority_ordering);
TEST("irqt_futex_wait_wake",   test_futex_wait_wake);
TEST("irqt_nanosleep_prec",    test_nanosleep_precision);
TEST("irqt_getpid_stable",    test_getpid_after_irq);
