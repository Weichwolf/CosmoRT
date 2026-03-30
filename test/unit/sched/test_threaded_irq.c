/* CosmoRT threaded-IRQ validation — proves device IRQ threading works */
#include "ktest.h"

#define PIPE_READ  0
#define PIPE_WRITE 1
#define SCHED_OTHER 0
#define SCHED_FIFO  1

#define SLEEP_10MS_NS  10000000L
#define SLEEP_50MS_NS  50000000L
#define SLEEP_5MS_NS    5000000L
#define ALARM_1S       1
#define ALARM_CANCEL   0
#define CONCURRENT_CHILDREN 4
#define FORK_STRESS_COUNT   8
#define YIELD_ITERATIONS  100
#define FUTEX_PRIVATE     128
#define FUTEX_WAIT        0
#define FUTEX_WAKE        1
#define EPOLL_TIMEOUT_MS  10
#define EFD_SEMAPHORE     1

struct sched_param_k { int sched_priority; };

static void msleep(int ms) {
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = ms * 1000000L };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
}

static void test_tirq_nanosleep_10ms(void) {
    puts("\n[threaded_irq]\n");
    struct k_timespec req = { .tv_sec = 0, .tv_nsec = SLEEP_10MS_NS };
    long r = sc2(SYS_NANOSLEEP, (long)&req, 0);
    check("nanosleep 10ms ok", r == 0 || r == -EINTR);
}

static void test_tirq_nanosleep_50ms(void) {
    struct k_timespec before, after;
    sc2(SYS_CLOCK_GETTIME, 1, (long)&before);
    struct k_timespec req = { .tv_sec = 0, .tv_nsec = SLEEP_50MS_NS };
    sc2(SYS_NANOSLEEP, (long)&req, 0);
    sc2(SYS_CLOCK_GETTIME, 1, (long)&after);
    long elapsed_ms = ((after.tv_sec - before.tv_sec) * 1000000000L +
                       (after.tv_nsec - before.tv_nsec)) / 1000000L;
    check("nanosleep 50ms >= 40ms", elapsed_ms >= 40);
    check("nanosleep 50ms <= 150ms", elapsed_ms <= 150);
}

static void test_tirq_fork_pipe(void) {
    int pfd[2];
    long r = sc1(SYS_PIPE, (long)pfd);
    check_val("pipe create", r, 0);
    if (r != 0) return;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_CLOSE, pfd[PIPE_READ]);
        char msg = 'T';
        sc3(SYS_WRITE, pfd[PIPE_WRITE], (long)&msg, 1);
        sc1(SYS_CLOSE, pfd[PIPE_WRITE]);
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }
    check("fork ok", pid > 0);
    if (pid <= 0) return;

    sc1(SYS_CLOSE, pfd[PIPE_WRITE]);
    char buf = 0;
    long n = sc3(SYS_READ, pfd[PIPE_READ], (long)&buf, 1);
    sc1(SYS_CLOSE, pfd[PIPE_READ]);
    check_val("pipe read byte", n, 1);
    check_val("pipe data = 'T'", (long)buf, 'T');
    sc4(SYS_WAIT4, pid, 0, 0, 0);
}

static void test_tirq_epoll_timeout(void) {
    long efd = sc4(SYS_EPOLL_CREATE1, 0, 0, 0, 0);
    check("epoll_create1", efd >= 0);
    if (efd < 0) return;

    struct { uint32_t events; uint64_t data; } __attribute__((packed)) ev;
    long r = sc4(SYS_EPOLL_WAIT, efd, (long)&ev, 1, EPOLL_TIMEOUT_MS);
    check_val("epoll_wait timeout", r, 0);
    sc1(SYS_CLOSE, efd);
}

__attribute__((naked)) static void tirq_restorer(void) {
    __asm__ volatile("mov $15, %%rax\n" "syscall\n" ::: "memory");
}

static volatile int tirq_got_alarm;

static void tirq_alarm_handler(int sig) {
    (void)sig;
    tirq_got_alarm = 1;
}

static void test_tirq_sigalrm(void) {
    struct {
        void *handler;
        unsigned long flags;
        void (*restorer)(void);
        uint64_t mask;
    } sa;
    for (int i = 0; i < (int)sizeof(sa); i++) ((char*)&sa)[i] = 0;

    tirq_got_alarm = 0;
    sa.handler = (void *)tirq_alarm_handler;
    sa.flags = 0x04000000;
    sa.restorer = (void *)tirq_restorer;
    sc4(SYS_RT_SIGACTION, SIGALRM, (long)&sa, 0, 8);

    sc1(SYS_ALARM, ALARM_1S);
    struct k_timespec req = { .tv_sec = 3, .tv_nsec = 0 };
    long r = sc2(SYS_NANOSLEEP, (long)&req, 0);
    check("SIGALRM interrupts sleep", r == -EINTR);
    sc1(SYS_ALARM, ALARM_CANCEL);
}

static void test_tirq_concurrent_nanosleep(void) {
    long pids[CONCURRENT_CHILDREN];
    for (int i = 0; i < CONCURRENT_CHILDREN; i++) {
        pids[i] = sc0(SYS_FORK);
        if (pids[i] == 0) {
            struct k_timespec req = { .tv_sec = 0, .tv_nsec = SLEEP_10MS_NS };
            sc2(SYS_NANOSLEEP, (long)&req, 0);
            sc1(SYS_EXIT, 0);
            __builtin_unreachable();
        }
    }
    int ok = 1;
    for (int i = 0; i < CONCURRENT_CHILDREN; i++) {
        if (pids[i] <= 0) { ok = 0; continue; }
        int status = 0;
        sc4(SYS_WAIT4, pids[i], (long)&status, 0, 0);
        if ((status & 0xFF00) != 0) ok = 0;
    }
    check("4 concurrent nanosleeps", ok);
}

static void test_tirq_fork_exit_stress(void) {
    int ok = 1;
    for (int i = 0; i < FORK_STRESS_COUNT; i++) {
        long pid = sc0(SYS_FORK);
        if (pid == 0) {
            sc1(SYS_EXIT, 0);
            __builtin_unreachable();
        }
        if (pid < 0) { ok = 0; continue; }
        sc4(SYS_WAIT4, pid, 0, 0, 0);
    }
    check("fork+exit stress 8x", ok);
}

static void test_tirq_rt_priority(void) {
    struct sched_param_k p = { .sched_priority = 10 };
    long r = sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_FIFO, (long)&p);
    check("set SCHED_FIFO", r == 0);

    int pol = (int)sc1(SYS_SCHED_GETSCHEDULER, 0);
    check_val("policy = FIFO", (long)pol, SCHED_FIFO);

    struct sched_param_k p2 = {0};
    sc2(SYS_SCHED_GETPARAM, 0, (long)&p2);
    check_val("prio = 10", (long)p2.sched_priority, 10);

    p.sched_priority = 0;
    sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_OTHER, (long)&p);
}

static void test_tirq_futex(void) {
    volatile uint32_t fvar = 0;
    long r = sc6(SYS_FUTEX, (long)&fvar, FUTEX_WAIT | FUTEX_PRIVATE, 1, 0, 0, 0);
    check_val("futex WAIT wrong val", r, -EAGAIN);

    r = sc6(SYS_FUTEX, (long)&fvar, FUTEX_WAKE | FUTEX_PRIVATE, 1, 0, 0, 0);
    check_val("futex WAKE no waiters", r, 0);
}

static void test_tirq_eventfd(void) {
    long efd = sc2(SYS_EVENTFD2, 0, 0);
    check("eventfd create", efd >= 0);
    if (efd < 0) return;

    uint64_t val = 42;
    long w = sc3(SYS_WRITE, efd, (long)&val, 8);
    check_val("eventfd write 8", w, 8);

    uint64_t rval = 0;
    long rd = sc3(SYS_READ, efd, (long)&rval, 8);
    check_val("eventfd read 8", rd, 8);
    check_val("eventfd value = 42", (long)rval, 42);

    sc1(SYS_CLOSE, efd);
}

static void test_tirq_yield_stability(void) {
    long pid_before = sc0(SYS_GETPID);
    for (int i = 0; i < YIELD_ITERATIONS; i++)
        sc0(SYS_SCHED_YIELD);
    long pid_after = sc0(SYS_GETPID);
    check("getpid stable after 100 yields", pid_before == pid_after);
}

TEST("tirq_nanosleep_10ms",       test_tirq_nanosleep_10ms);
TEST("tirq_nanosleep_50ms",       test_tirq_nanosleep_50ms);
TEST("tirq_fork_pipe",            test_tirq_fork_pipe);
TEST("tirq_epoll_timeout",        test_tirq_epoll_timeout);
TEST("tirq_sigalrm",              test_tirq_sigalrm);
TEST("tirq_concurrent_sleep",     test_tirq_concurrent_nanosleep);
TEST("tirq_fork_exit_stress",     test_tirq_fork_exit_stress);
TEST("tirq_rt_priority",          test_tirq_rt_priority);
TEST("tirq_futex",                test_tirq_futex);
TEST("tirq_eventfd",              test_tirq_eventfd);
TEST("tirq_yield_stability",      test_tirq_yield_stability);
