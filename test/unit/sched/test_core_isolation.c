/* CosmoRT — Core-Isolation integration tests */
#include "ktest.h"

#define SCHED_OTHER  0
#define SCHED_FIFO   1
#define SA_RESTORER  0x04000000
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

static void test_num_cores(void) {
    puts("\n[Core Isolation]\n");
    long pid = sc0(SYS_FORK);
    if (pid == 0) sc1(SYS_EXIT, 0);
    check("iso_num_cores_ge2", pid > 0);
    wait_child(pid);
}

static void test_fork_pipe(void) {
    int fds[2];
    check("iso_pipe_create", sc2(SYS_PIPE2, (long)fds, 0) == 0);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        char c = 'I';
        sc3(SYS_WRITE, fds[1], (long)&c, 1);
        sc1(SYS_EXIT, 0);
    }
    char buf = 0;
    sc3(SYS_READ, fds[0], (long)&buf, 1);
    check("iso_pipe_roundtrip", buf == 'I');
    check("iso_pipe_child", wait_child(pid) == 0);
    sc1(SYS_CLOSE, fds[0]);
    sc1(SYS_CLOSE, fds[1]);
}

static void test_nanosleep_10ms(void) {
    long r = msleep(10);
    check_val("iso_nanosleep_10ms", r, 0);
}

static void test_sched_fifo(void) {
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct sched_param p = { .sched_priority = 10 };
        long r = sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_FIFO, (long)&p);
        sc0(SYS_SCHED_YIELD);
        sc1(SYS_EXIT, (r == 0) ? 0 : 1);
    }
    check_val("iso_sched_fifo", wait_child(pid), 0);
}

static void test_concurrent_forks(void) {
    long pids[4];
    for (int i = 0; i < 4; i++) {
        pids[i] = sc0(SYS_FORK);
        if (pids[i] == 0) {
            msleep(5);
            sc1(SYS_EXIT, 0);
        }
    }
    int ok = 1;
    for (int i = 0; i < 4; i++)
        if (wait_child(pids[i]) != 0) ok = 0;
    check("iso_concurrent_forks_4", ok);
}

static void test_rt_ordering(void) {
    volatile int *seq = (volatile int *)(long)sc6(SYS_MMAP, 0, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if ((long)seq < 0) { fail("iso_rt_ordering", "mmap failed"); return; }
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
    check("iso_rt_high_before_low", seq[2] > 0 && (seq[1] == 0 || seq[2] < seq[1]));
    sc2(SYS_MUNMAP, (long)seq, 4096);
}

static void test_epoll_timeout(void) {
    long efd = sc1(SYS_EPOLL_CREATE1, 0);
    check("iso_epoll_create", efd >= 0);
    if (efd < 0) return;
    struct epoll_event out;
    long r = sc4(SYS_EPOLL_WAIT, efd, (long)&out, 1, 20);
    check_val("iso_epoll_wait_timeout", r, 0);
    sc1(SYS_CLOSE, efd);
}

static void test_dns_socket(void) {
    long s = sc3(SYS_SOCKET, 2, 2, 17);
    check("iso_udp_socket", s >= 0);
    if (s >= 0) sc1(SYS_CLOSE, s);
}

static void test_futex(void) {
    volatile int *val = (volatile int *)(long)sc6(SYS_MMAP, 0, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if ((long)val < 0) { fail("iso_futex", "mmap failed"); return; }
    *val = 0;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        msleep(10);
        *val = 1;
        sc6(SYS_FUTEX, (long)val, FUTEX_WAKE, 1, 0, 0, 0);
        sc1(SYS_EXIT, 0);
    }

    sc6(SYS_FUTEX, (long)val, FUTEX_WAIT, 0, 0, 0, 0);
    check("iso_futex_woken", *val == 1);
    check("iso_futex_child", wait_child(pid) == 0);
    sc2(SYS_MUNMAP, (long)val, 4096);
}

static void test_yield_stress(void) {
    for (int i = 0; i < 100; i++)
        sc0(SYS_SCHED_YIELD);
    pass("iso_yield_100x");
}

static void test_fork_exit_stress(void) {
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        long pid = sc0(SYS_FORK);
        if (pid == 0) sc1(SYS_EXIT, 0);
        if (wait_child(pid) != 0) ok = 0;
    }
    check("iso_fork_exit_8x", ok);
}

static void test_nanosleep_50ms(void) {
    long r = msleep(50);
    check_val("iso_nanosleep_50ms", r, 0);
}

TEST("core_isolation", test_num_cores);
TEST("core_isolation", test_fork_pipe);
TEST("core_isolation", test_nanosleep_10ms);
TEST("core_isolation", test_sched_fifo);
TEST("core_isolation", test_concurrent_forks);
TEST("core_isolation", test_rt_ordering);
TEST("core_isolation", test_epoll_timeout);
TEST("core_isolation", test_dns_socket);
TEST("core_isolation", test_futex);
TEST("core_isolation", test_yield_stress);
TEST("core_isolation", test_fork_exit_stress);
TEST("core_isolation", test_nanosleep_50ms);
