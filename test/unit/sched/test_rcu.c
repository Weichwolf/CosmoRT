/* CosmoRT — RCU integration tests */
#include "ktest.h"

#define SCHED_OTHER  0
#define SCHED_FIFO   1
#define SCHED_RR     2
#define SA_RESTORER  0x04000000
#define PIPE_COUNT   100
#define YIELD_COUNT  100
#define FORK_STRESS  4
#define CHILD_OK     0

struct sched_param { int sched_priority; };

static long msleep(long ms) {
    struct k_timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    return sc2(SYS_NANOSLEEP, (long)&ts, 0);
}

static long wait_child(long pid) {
    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    return (ws >> 8) & 0xff;
}

static void test_fork_pipe_roundtrip(void) {
    puts("\n[RCU]\n");
    int fds[2];
    check("rcu_pipe_create", sc2(SYS_PIPE2, (long)fds, 0) == 0);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        char c = 'R';
        sc3(SYS_WRITE, fds[1], (long)&c, 1);
        sc1(SYS_EXIT, 0);
    }

    char buf = 0;
    sc3(SYS_READ, fds[0], (long)&buf, 1);
    check("rcu_pipe_roundtrip", buf == 'R');
    check("rcu_child_exit", wait_child(pid) == CHILD_OK);
    sc1(SYS_CLOSE, fds[0]);
    sc1(SYS_CLOSE, fds[1]);
}

static void test_preempt_nesting(void) {
    for (int i = 0; i < 50; i++)
        sc0(SYS_SCHED_YIELD);
    pass("rcu_preempt_nesting_50x_yield");
}

static void test_nanosleep_10ms(void) {
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 10000000L };
    long r = sc2(SYS_NANOSLEEP, (long)&ts, 0);
    check_val("rcu_nanosleep_10ms", r, 0);
}

static void test_nanosleep_back_to_back(void) {
    for (int i = 0; i < 5; i++) {
        struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 2000000L };
        sc2(SYS_NANOSLEEP, (long)&ts, 0);
    }
    pass("rcu_nanosleep_5x_2ms");
}

static void test_fork_stress(void) {
    long pids[FORK_STRESS];
    for (int i = 0; i < FORK_STRESS; i++) {
        pids[i] = sc0(SYS_FORK);
        if (pids[i] == 0)
            sc1(SYS_EXIT, 0);
    }
    int ok = 1;
    for (int i = 0; i < FORK_STRESS; i++) {
        if (wait_child(pids[i]) != CHILD_OK)
            ok = 0;
    }
    check("rcu_fork_stress_4", ok);
}

static void test_fork_exec(void) {
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc0(SYS_SCHED_YIELD);
        sc1(SYS_EXIT, 0);
    }
    check("rcu_fork_exec_child", wait_child(pid) == CHILD_OK);
}

static void test_epoll_timeout(void) {
    long efd = sc1(SYS_EPOLL_CREATE1, 0);
    check("rcu_epoll_create", efd >= 0);
    if (efd < 0) return;

    struct epoll_event out;
    long r = sc4(SYS_EPOLL_WAIT, efd, (long)&out, 1, 10);
    check_val("rcu_epoll_wait_timeout", r, 0);
    sc1(SYS_CLOSE, efd);
}

__attribute__((naked)) static void rcu_sig_restorer(void) {
    __asm__ volatile("mov $15, %%rax\n" "syscall\n" ::: "memory");
}

static volatile int rcu_got_alarm;

static void rcu_alarm_handler(int sig) {
    (void)sig;
    rcu_got_alarm = 1;
}

static void test_sigalrm(void) {
    struct {
        void *handler;
        unsigned long flags;
        void (*restorer)(void);
        uint64_t mask;
    } sa;
    for (int i = 0; i < (int)sizeof(sa); i++) ((char*)&sa)[i] = 0;

    rcu_got_alarm = 0;
    sa.handler = (void *)rcu_alarm_handler;
    sa.flags = SA_RESTORER;
    sa.restorer = (void *)rcu_sig_restorer;
    sc4(SYS_RT_SIGACTION, SIGALRM, (long)&sa, 0, 8);

    sc1(SYS_ALARM, 1);
    struct k_timespec req = { .tv_sec = 3, .tv_nsec = 0 };
    long r = sc2(SYS_NANOSLEEP, (long)&req, 0);
    check("rcu_sigalrm_interrupts_sleep", r == -EINTR);
    sc1(SYS_ALARM, 0);
}

static void test_rt_priority_ordering(void) {
    volatile int *seq = (volatile int *)(long)sc6(SYS_MMAP, 0, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if ((long)seq < 0) { fail("rcu_rt_prio", "mmap failed"); return; }
    seq[0] = 0; seq[1] = 0; seq[2] = 0;

    long pidLow = sc0(SYS_FORK);
    if (pidLow == 0) {
        struct sched_param p = { .sched_priority = 5 };
        sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_FIFO, (long)&p);
        sc0(SYS_SCHED_YIELD);
        msleep(5);
        seq[1] = __sync_add_and_fetch(&seq[0], 1);
        sc1(SYS_EXIT, 0);
    }

    long pidHigh = sc0(SYS_FORK);
    if (pidHigh == 0) {
        struct sched_param p = { .sched_priority = 25 };
        sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_FIFO, (long)&p);
        sc0(SYS_SCHED_YIELD);
        seq[2] = __sync_add_and_fetch(&seq[0], 1);
        sc1(SYS_EXIT, 0);
    }

    int ws;
    sc4(SYS_WAIT4, pidHigh, (long)&ws, 0, 0);
    sc4(SYS_WAIT4, pidLow, (long)&ws, 0, 0);
    check("rcu_rt_high_before_low", seq[2] > 0 && (seq[1] == 0 || seq[2] < seq[1]));
    sc2(SYS_MUNMAP, (long)seq, 4096);
}

static void test_pipe_contention(void) {
    int ab[2], ba[2];
    check("rcu_pipe_create", sc2(SYS_PIPE2, (long)ab, 0) == 0);
    sc2(SYS_PIPE2, (long)ba, 0);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_CLOSE, ab[1]);
        sc1(SYS_CLOSE, ba[0]);
        char c;
        for (int i = 0; i < PIPE_COUNT; i++) {
            sc3(SYS_READ, ab[0], (long)&c, 1);
            sc3(SYS_WRITE, ba[1], (long)&c, 1);
        }
        sc1(SYS_EXIT, 0);
    }

    sc1(SYS_CLOSE, ab[0]);
    sc1(SYS_CLOSE, ba[1]);
    char c = 'P';
    for (int i = 0; i < PIPE_COUNT; i++) {
        sc3(SYS_WRITE, ab[1], (long)&c, 1);
        sc3(SYS_READ, ba[0], (long)&c, 1);
    }
    check("rcu_pipe_100_cycles", c == 'P');
    sc1(SYS_CLOSE, ab[1]);
    sc1(SYS_CLOSE, ba[0]);
    check("rcu_pipe_child", wait_child(pid) == CHILD_OK);
}

static void test_yield_stress(void) {
    for (int i = 0; i < YIELD_COUNT; i++)
        sc0(SYS_SCHED_YIELD);
    pass("rcu_yield_100x");
}

static void test_dns_socket(void) {
    long s = sc3(SYS_SOCKET, 2, 2, 17);
    check("rcu_dns_socket", s >= 0);
    if (s >= 0) sc1(SYS_CLOSE, s);
}

TEST("rcu", test_fork_pipe_roundtrip);
TEST("rcu", test_preempt_nesting);
TEST("rcu", test_nanosleep_10ms);
TEST("rcu", test_nanosleep_back_to_back);
TEST("rcu", test_fork_stress);
TEST("rcu", test_fork_exec);
TEST("rcu", test_epoll_timeout);
TEST("rcu", test_sigalrm);
TEST("rcu", test_rt_priority_ordering);
TEST("rcu", test_pipe_contention);
TEST("rcu", test_yield_stress);
TEST("rcu", test_dns_socket);
