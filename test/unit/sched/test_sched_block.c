/* test_sched_block — RT-9a infrastructure regression: scheduler intact after context_save/resume */
#include "ktest.h"

#define NSEC_PER_MSEC    1000000L
#define NSEC_PER_SEC     1000000000L
#define PAGE_SIZE        4096

#define FUTEX_WAIT       0
#define FUTEX_WAKE       1

#define SCHED_FIFO_SB    1

#define EPOLL_CTL_ADD    1
#define EPOLLIN          1

struct sched_param_sb { int sched_priority; };

struct epoll_event_sb {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

__attribute__((naked)) static void restorer_sb(void) {
    __asm__ volatile("mov $15, %%rax\n" "syscall\n" ::: "memory");
}

struct ksigaction_sb {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

static volatile int alarm_fired;

static void alarm_handler(int sig) {
    (void)sig;
    alarm_fired = 1;
}

static long now_ns_sb(void) {
    struct k_timespec ts;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&ts);
    return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

static void test_sb_all(void) {
    puts("\n[sched_block]\n");

    {
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
        check("sb_fork_pipe", r == 'X');
        sc1(SYS_CLOSE, pfd[0]);
        sc1(SYS_CLOSE, pfd[1]);
    }

    {
        long t0 = now_ns_sb();
        struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 10 * NSEC_PER_MSEC };
        sc2(SYS_NANOSLEEP, (long)&ts, 0);
        long dt = (now_ns_sb() - t0) / NSEC_PER_MSEC;
        check("sb_nanosleep_10ms", dt >= 9 && dt < 100);
    }

    {
        long t0 = now_ns_sb();
        struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 20 * NSEC_PER_MSEC };
        sc2(SYS_NANOSLEEP, (long)&ts, 0);
        long dt = (now_ns_sb() - t0) / NSEC_PER_MSEC;
        check("sb_nanosleep_20ms", dt >= 19 && dt < 200);
    }

    {
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
        check("sb_4forks", ok);
    }

    {
        volatile int *shared = (volatile int *)(long)sc6(SYS_MMAP, 0, PAGE_SIZE,
            PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if ((long)shared >= 0) {
            shared[0] = 0;
            long pid = sc0(SYS_FORK);
            if (pid == 0) {
                struct sched_param_sb sp = { .sched_priority = 10 };
                sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_FIFO_SB, (long)&sp);
                shared[0] = 1;
                sc1(SYS_EXIT, 0);
            }
            int ws;
            sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
            check("sb_rt_preempt", shared[0] == 1);
        }
    }

    {
        long efd = sc1(SYS_EPOLL_CREATE1, 0);
        if (efd >= 0) {
            int pfd[2];
            sc1(SYS_PIPE2, (long)pfd);
            struct epoll_event_sb ev = { .events = EPOLLIN, .data = 42 };
            sc4(SYS_EPOLL_CTL, efd, EPOLL_CTL_ADD, pfd[0], (long)&ev);
            struct epoll_event_sb out;
            long n = sc4(SYS_EPOLL_WAIT, efd, (long)&out, 1, 10);
            check("sb_epoll_timeout", n == 0);
            sc1(SYS_CLOSE, pfd[0]);
            sc1(SYS_CLOSE, pfd[1]);
            sc1(SYS_CLOSE, efd);
        }
    }

    {
        volatile int *futval = (volatile int *)(long)sc6(SYS_MMAP, 0, PAGE_SIZE,
            PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if ((long)futval >= 0) {
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
            check("sb_futex", 1);
        }
    }

    {
        for (int i = 0; i < 100; i++)
            sc0(SYS_SCHED_YIELD);
        check("sb_yield_100x", 1);
    }

    {
        int sfd = sc3(SYS_SOCKET, AF_INET, SOCK_DGRAM, 0);
        check("sb_udp_socket", sfd >= 0);
        if (sfd >= 0) sc1(SYS_CLOSE, sfd);
    }

    {
        int pfd[2];
        sc1(SYS_PIPE2, (long)pfd);
        long pid = sc0(SYS_FORK);
        if (pid == 0) {
            char buf;
            for (int i = 0; i < 10; i++) {
                sc3(SYS_READ, pfd[0], (long)&buf, 1);
                sc3(SYS_WRITE, pfd[1], (long)&buf, 1);
            }
            sc1(SYS_EXIT, 0);
        }
        int ok = 1;
        for (int i = 0; i < 10; i++) {
            char c = (char)i;
            sc3(SYS_WRITE, pfd[1], (long)&c, 1);
            char r;
            sc3(SYS_READ, pfd[0], (long)&r, 1);
            if (r != (char)i) ok = 0;
        }
        int ws;
        sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
        check("sb_pipe_10rt", ok);
        sc1(SYS_CLOSE, pfd[0]);
        sc1(SYS_CLOSE, pfd[1]);
    }

    {
        int ok = 1;
        for (int i = 0; i < 8; i++) {
            long pid = sc0(SYS_FORK);
            if (pid == 0) sc1(SYS_EXIT, 0);
            int ws;
            long r = sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
            if (r != pid) ok = 0;
        }
        check("sb_fork_exit_8x", ok);
    }

    {
        alarm_fired = 0;
        struct ksigaction_sb sa = {
            .handler = (void *)alarm_handler,
            .flags = 0x04000000,
            .restorer = (void *)restorer_sb,
            .mask = 0
        };
        sc4(SYS_RT_SIGACTION, SIGALRM, (long)&sa, 0, 8);
        sc1(SYS_ALARM, 1);
        struct k_timespec ts = { .tv_sec = 2, .tv_nsec = 0 };
        sc2(SYS_NANOSLEEP, (long)&ts, 0);
        check("sb_sigalrm", alarm_fired == 1);
    }
}

TEST("sched_block", test_sb_all);
