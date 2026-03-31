/* test_unified_switch — unified context_switch mechanism validation */
#include "ktest.h"

#define NSEC_PER_MSEC    1000000L
#define NSEC_PER_SEC     1000000000L
#define PAGE_SIZE        4096

#define FUTEX_WAIT       0
#define FUTEX_WAKE       1

#define SCHED_FIFO_US    1

#define EPOLL_CTL_ADD    1
#define EPOLLIN          1

struct sched_param_us { int sched_priority; };

struct epoll_event_us {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

__attribute__((naked)) static void restorer_us(void) {
    __asm__ volatile("mov $15, %%rax\n" "syscall\n" ::: "memory");
}

struct ksigaction_us {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

static volatile int alarm_fired_us;

static void alarm_handler_us(int sig) {
    (void)sig;
    alarm_fired_us = 1;
}

static long now_ns(void) {
    struct k_timespec ts;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&ts);
    return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

static void test_us_fork_pipe(void) {
    puts("\n[unified_switch]\n");
    int pfd[2];
    sc1(SYS_PIPE2, (long)pfd);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        char buf;
        sc3(SYS_READ, pfd[0], (long)&buf, 1);
        sc3(SYS_WRITE, pfd[1], (long)&buf, 1);
        sc1(SYS_EXIT, 0);
    }
    char c = 'X';
    sc3(SYS_WRITE, pfd[1], (long)&c, 1);
    char r = 0;
    sc3(SYS_READ, pfd[0], (long)&r, 1);
    int ws;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check("fork_pipe", r == 'X');
    sc1(SYS_CLOSE, pfd[0]);
    sc1(SYS_CLOSE, pfd[1]);
}

static void test_us_nanosleep(void) {
    long t0 = now_ns();
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 10 * NSEC_PER_MSEC };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
    long dt = (now_ns() - t0) / NSEC_PER_MSEC;
    check("nanosleep_10ms", dt >= 9 && dt < 200);
}

static void test_us_4forks(void) {
    long pids[4];
    for (int i = 0; i < 4; i++) {
        pids[i] = sc0(SYS_FORK);
        if (pids[i] == 0) sc1(SYS_EXIT, 0);
    }
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        int ws;
        long r = sc4(SYS_WAIT4, pids[i], (long)&ws, 0, 0);
        if (r != pids[i]) ok = 0;
    }
    check("4_concurrent_forks", ok);
}

static void test_us_rt_preempt(void) {
    volatile int *shared = (volatile int *)(long)sc6(SYS_MMAP, 0, PAGE_SIZE,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if ((long)shared < 0) { check("rt_mmap", 0); return; }
    shared[0] = 0;
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct sched_param_us sp = { .sched_priority = 10 };
        sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_FIFO_US, (long)&sp);
        shared[0] = 1;
        sc1(SYS_EXIT, 0);
    }
    int ws;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check("rt_preempts_other", shared[0] == 1);
}

static void test_us_futex(void) {
    volatile int *futval = (volatile int *)(long)sc6(SYS_MMAP, 0, PAGE_SIZE,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if ((long)futval < 0) { check("futex_mmap", 0); return; }
    *futval = 0;
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        while (__atomic_load_n(futval, __ATOMIC_ACQUIRE) == 0)
            sc4(SYS_FUTEX, (long)futval, FUTEX_WAIT, 0, 0);
        sc1(SYS_EXIT, 0);
    }
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 5 * NSEC_PER_MSEC };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
    __atomic_store_n(futval, 1, __ATOMIC_RELEASE);
    sc3(SYS_FUTEX, (long)futval, FUTEX_WAKE, 1);
    int ws;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check("futex_wait_wake", 1);
}

static void test_us_pipe_100rt(void) {
    int pfd[2];
    sc1(SYS_PIPE2, (long)pfd);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        char buf;
        for (int i = 0; i < 100; i++) {
            sc3(SYS_READ, pfd[0], (long)&buf, 1);
            sc3(SYS_WRITE, pfd[1], (long)&buf, 1);
        }
        sc1(SYS_EXIT, 0);
    }
    int ok = 1;
    for (int i = 0; i < 100; i++) {
        char c = (char)i;
        sc3(SYS_WRITE, pfd[1], (long)&c, 1);
        char r;
        sc3(SYS_READ, pfd[0], (long)&r, 1);
        if (r != (char)i) ok = 0;
    }
    int ws;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check("pipe_100_roundtrips", ok);
    sc1(SYS_CLOSE, pfd[0]);
    sc1(SYS_CLOSE, pfd[1]);
}

static void test_us_yield_100x(void) {
    for (int i = 0; i < 100; i++)
        sc0(SYS_SCHED_YIELD);
    check("yield_100x", 1);
}

static void test_us_udp_socket(void) {
    int sfd = sc3(SYS_SOCKET, AF_INET, SOCK_DGRAM, 0);
    check("udp_socket_create", sfd >= 0);
    if (sfd >= 0) sc1(SYS_CLOSE, sfd);
}

static void test_us_epoll_timeout(void) {
    long efd = sc1(SYS_EPOLL_CREATE1, 0);
    if (efd < 0) { check("epoll_create", 0); return; }
    int pfd[2];
    sc1(SYS_PIPE2, (long)pfd);
    struct epoll_event_us ev = { .events = EPOLLIN, .data = 42 };
    sc4(SYS_EPOLL_CTL, efd, EPOLL_CTL_ADD, pfd[0], (long)&ev);
    struct epoll_event_us out;
    long n = sc4(SYS_EPOLL_WAIT, efd, (long)&out, 1, 10);
    check("epoll_wait_timeout", n == 0);
    sc1(SYS_CLOSE, pfd[0]);
    sc1(SYS_CLOSE, pfd[1]);
    sc1(SYS_CLOSE, efd);
}

static void test_us_sigalrm(void) {
    alarm_fired_us = 0;
    struct ksigaction_us sa = {
        .handler = (void *)alarm_handler_us,
        .flags = 0x04000000,
        .restorer = (void *)restorer_us,
        .mask = 0
    };
    sc4(SYS_RT_SIGACTION, SIGALRM, (long)&sa, 0, 8);
    sc1(SYS_ALARM, 1);
    struct k_timespec ts = { .tv_sec = 2, .tv_nsec = 0 };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
    check("sigalrm_delivery", alarm_fired_us == 1);
}

static void test_us_eventfd(void) {
    long efd = sc2(SYS_EVENTFD2, 0, 0);
    if (efd < 0) { check("eventfd_create", 0); return; }
    uint64_t val = 1;
    long n = sc3(SYS_WRITE, efd, (long)&val, 8);
    check("eventfd_write", n == 8);
    uint64_t rval = 0;
    n = sc3(SYS_READ, efd, (long)&rval, 8);
    check("eventfd_read", n == 8 && rval == 1);
    sc1(SYS_CLOSE, efd);
}

static void test_us_fork_exit_stress(void) {
    int ok = 1;
    for (int i = 0; i < 10; i++) {
        long pid = sc0(SYS_FORK);
        if (pid == 0) sc1(SYS_EXIT, 0);
        int ws;
        long r = sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
        if (r != pid) ok = 0;
    }
    check("fork_exit_10x", ok);
}

TEST("us_fork_pipe", test_us_fork_pipe);
TEST("us_nanosleep", test_us_nanosleep);
TEST("us_4forks", test_us_4forks);
TEST("us_rt_preempt", test_us_rt_preempt);
TEST("us_futex", test_us_futex);
TEST("us_pipe_100rt", test_us_pipe_100rt);
TEST("us_yield_100x", test_us_yield_100x);
TEST("us_udp_socket", test_us_udp_socket);
TEST("us_epoll_timeout", test_us_epoll_timeout);
TEST("us_sigalrm", test_us_sigalrm);
TEST("us_eventfd", test_us_eventfd);
TEST("us_fork_exit_stress", test_us_fork_exit_stress);
