/* CosmoRT — NOHZ_FULL integration tests */
#include "ktest.h"

#define SCHED_OTHER  0
#define SCHED_FIFO   1
#define SA_RESTORER  0x04000000
#define CHILD_OK     0
#define FUTEX_WAIT   0
#define FUTEX_WAKE   1

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

static void test_nanosleep_10ms(void) {
    puts("\n[NOHZ]\n");
    long r = msleep(10);
    check_val("nohz_nanosleep_10ms", r, 0);
}

static void test_nanosleep_50ms(void) {
    long r = msleep(50);
    check_val("nohz_nanosleep_50ms", r, 0);
}

static void test_fork_pipe_roundtrip(void) {
    int fds[2];
    check("nohz_pipe_create", sc2(SYS_PIPE2, (long)fds, 0) == 0);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        char c = 'N';
        sc3(SYS_WRITE, fds[1], (long)&c, 1);
        sc1(SYS_EXIT, 0);
    }

    char buf = 0;
    sc3(SYS_READ, fds[0], (long)&buf, 1);
    check("nohz_pipe_roundtrip", buf == 'N');
    check("nohz_pipe_child", wait_child(pid) == CHILD_OK);
    sc1(SYS_CLOSE, fds[0]);
    sc1(SYS_CLOSE, fds[1]);
}

static void test_concurrent_nanosleeps(void) {
    long pids[4];
    for (int i = 0; i < 4; i++) {
        pids[i] = sc0(SYS_FORK);
        if (pids[i] == 0) {
            msleep(10 + i * 5);
            sc1(SYS_EXIT, 0);
        }
    }
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        if (wait_child(pids[i]) != CHILD_OK)
            ok = 0;
    }
    check("nohz_concurrent_sleeps_4", ok);
}

static void test_epoll_wait_timeout(void) {
    long efd = sc1(SYS_EPOLL_CREATE1, 0);
    check("nohz_epoll_create", efd >= 0);
    if (efd < 0) return;

    struct epoll_event out;
    long r = sc4(SYS_EPOLL_WAIT, efd, (long)&out, 1, 20);
    check_val("nohz_epoll_wait_timeout", r, 0);
    sc1(SYS_CLOSE, efd);
}

__attribute__((naked)) static void nohz_sig_restorer(void) {
    __asm__ volatile("mov $15, %%rax\n" "syscall\n" ::: "memory");
}

static volatile int nohz_got_alarm;

static void nohz_alarm_handler(int sig) {
    (void)sig;
    nohz_got_alarm = 1;
}

static void test_sigalrm(void) {
    struct {
        void *handler;
        unsigned long flags;
        void (*restorer)(void);
        uint64_t mask;
    } sa;
    for (int i = 0; i < (int)sizeof(sa); i++) ((char *)&sa)[i] = 0;

    nohz_got_alarm = 0;
    sa.handler = (void *)nohz_alarm_handler;
    sa.flags = SA_RESTORER;
    sa.restorer = (void *)nohz_sig_restorer;
    sc4(SYS_RT_SIGACTION, SIGALRM, (long)&sa, 0, 8);

    sc1(SYS_ALARM, 1);
    struct k_timespec req = { .tv_sec = 3, .tv_nsec = 0 };
    long r = sc2(SYS_NANOSLEEP, (long)&req, 0);
    check("nohz_sigalrm_interrupts_sleep", r == -EINTR);
    sc1(SYS_ALARM, 0);
}

static void test_dns_socket(void) {
    long s = sc3(SYS_SOCKET, 2, 2, 17);
    check("nohz_udp_socket", s >= 0);
    if (s >= 0) sc1(SYS_CLOSE, s);
}

static void test_rt_priority(void) {
    volatile int *seq = (volatile int *)(long)sc6(SYS_MMAP, 0, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if ((long)seq < 0) { fail("nohz_rt_prio", "mmap failed"); return; }
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
    check("nohz_rt_high_before_low", seq[2] > 0 && (seq[1] == 0 || seq[2] < seq[1]));
    sc2(SYS_MUNMAP, (long)seq, 4096);
}

static void test_sched_yield_stress(void) {
    for (int i = 0; i < 100; i++)
        sc0(SYS_SCHED_YIELD);
    pass("nohz_yield_100x");
}

static void test_futex_wait_wake(void) {
    volatile int *futex_val = (volatile int *)(long)sc6(SYS_MMAP, 0, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if ((long)futex_val < 0) { fail("nohz_futex", "mmap failed"); return; }
    *futex_val = 0;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        msleep(10);
        *futex_val = 1;
        sc6(SYS_FUTEX, (long)futex_val, FUTEX_WAKE, 1, 0, 0, 0);
        sc1(SYS_EXIT, 0);
    }

    sc6(SYS_FUTEX, (long)futex_val, FUTEX_WAIT, 0, 0, 0, 0);
    check("nohz_futex_woken", *futex_val == 1);
    check("nohz_futex_child", wait_child(pid) == CHILD_OK);
    sc2(SYS_MUNMAP, (long)futex_val, 4096);
}

static void test_fork_exit_stress(void) {
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        long pid = sc0(SYS_FORK);
        if (pid == 0) sc1(SYS_EXIT, 0);
        if (wait_child(pid) != CHILD_OK) ok = 0;
    }
    check("nohz_fork_exit_8x", ok);
}

static void test_tcp_connect(void) {
    long s = sc3(SYS_SOCKET, 2, 1, 6);
    check("nohz_tcp_socket", s >= 0);
    if (s >= 0) sc1(SYS_CLOSE, s);
}

TEST("nohz", test_nanosleep_10ms);
TEST("nohz", test_nanosleep_50ms);
TEST("nohz", test_fork_pipe_roundtrip);
TEST("nohz", test_concurrent_nanosleeps);
TEST("nohz", test_epoll_wait_timeout);
TEST("nohz", test_sigalrm);
TEST("nohz", test_dns_socket);
TEST("nohz", test_rt_priority);
TEST("nohz", test_sched_yield_stress);
TEST("nohz", test_futex_wait_wake);
TEST("nohz", test_fork_exit_stress);
TEST("nohz", test_tcp_connect);
