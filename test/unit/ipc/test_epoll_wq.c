/* Phase 10.2b-5 — epoll on wait_queue_entry.func / ep_poll_callback.
 *
 * Each epitem registers on the source fd's wq with func=ep_poll_callback;
 * source wakes ep->wq via the callback. epoll_wait blocks on ep->wq with
 * DEFINE_WAIT (no per-core sleeper array, no epoll_wake_all). Validates:
 *   - blocking epoll_wait wakes when an eventfd write fires
 *   - blocking epoll_wait wakes when a pipe write fires
 *   - blocking epoll_wait returns -EINTR on signal
 *   - epoll_wait with finite timeout returns 0 when nothing fires
 */
#include "ktest.h"

struct ksigaction_ep {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

__attribute__((naked)) static void ep_sig_restorer(void) {
    __asm__ volatile("mov $15, %%rax\nsyscall\n" ::: "memory");
}

static volatile int ep_sig_received;
static void ep_sig_handler(int sig) { (void)sig; ep_sig_received = 1; }

static void install_ep_handler(int signo, void (*fn)(int)) {
    struct ksigaction_ep sa = {
        .handler  = (void *)fn,
        .flags    = SA_RESTORER,
        .restorer = (void *)ep_sig_restorer,
        .mask     = 0,
    };
    sc4(SYS_RT_SIGACTION, signo, (long)&sa, 0, 8);
}

/* ── 01: blocking epoll_wait wakes when eventfd is written ── */

static void test_epoll_wait_eventfd_wakeup(void) {
    puts("\n[epoll_wq]\n");

    long efd = sc2(SYS_EVENTFD2, 0, 0);
    check("eventfd2 >= 0", efd >= 0);
    if (efd < 0) return;
    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1 >= 0", epfd >= 0);
    if (epfd < 0) { sc1(SYS_CLOSE, efd); return; }

    struct epoll_event ev = { .events = EPOLLIN, .data = 0xCAFE };
    long r = sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, efd, (long)&ev);
    check_val("epoll_ctl ADD ok", r, 0);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct epoll_event out;
        /* Block up to 2s; sibling will write to efd ~30ms in. */
        long n = sc4(SYS_EPOLL_WAIT, epfd, (long)&out, 1, 2000);
        int code = 0;
        if (n == 1) code |= 1;
        if ((out.events & EPOLLIN) != 0) code |= 2;
        if (out.data == 0xCAFE) code |= 4;
        sc1(SYS_EXIT, code);
        __builtin_unreachable();
    }
    /* Give child time to enter epoll_wait. */
    struct k_timespec delay = { .tv_sec = 0, .tv_nsec = 50000000 /* 50ms */ };
    sc2(SYS_NANOSLEEP, (long)&delay, 0);
    uint64_t one = 1;
    sc3(SYS_WRITE, efd, (long)&one, 8);

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    int code = (ws >> 8) & 0xFF;
    check("eventfd wakeup: epoll_wait returned 1",   (code & 1) == 1);
    check("eventfd wakeup: EPOLLIN delivered",       (code & 2) == 2);
    check("eventfd wakeup: data correct",            (code & 4) == 4);

    sc1(SYS_CLOSE, epfd);
    sc1(SYS_CLOSE, efd);
}

/* ── 02: blocking epoll_wait wakes when pipe write fires ── */

static void test_epoll_wait_pipe_wakeup(void) {
    int fds[2] = { -1, -1 };
    long r = sc2(SYS_PIPE2, (long)fds, 0);
    check_val("pipe2 ok", r, 0);
    if (r != 0) return;
    int rfd = fds[0], wfd = fds[1];

    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1 >= 0", epfd >= 0);
    if (epfd < 0) { sc1(SYS_CLOSE, rfd); sc1(SYS_CLOSE, wfd); return; }

    struct epoll_event ev = { .events = EPOLLIN, .data = 0xBEEF };
    r = sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, rfd, (long)&ev);
    check_val("epoll_ctl ADD pipe ok", r, 0);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct epoll_event out;
        long n = sc4(SYS_EPOLL_WAIT, epfd, (long)&out, 1, 2000);
        int code = 0;
        if (n == 1) code |= 1;
        if ((out.events & EPOLLIN) != 0) code |= 2;
        if (out.data == 0xBEEF) code |= 4;
        sc1(SYS_EXIT, code);
        __builtin_unreachable();
    }
    struct k_timespec delay = { .tv_sec = 0, .tv_nsec = 50000000 };
    sc2(SYS_NANOSLEEP, (long)&delay, 0);
    sc3(SYS_WRITE, wfd, (long)"x", 1);

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    int code = (ws >> 8) & 0xFF;
    check("pipe wakeup: epoll_wait returned 1", (code & 1) == 1);
    check("pipe wakeup: EPOLLIN delivered",     (code & 2) == 2);
    check("pipe wakeup: data correct",          (code & 4) == 4);

    sc1(SYS_CLOSE, epfd);
    sc1(SYS_CLOSE, rfd);
    sc1(SYS_CLOSE, wfd);
}

/* ── 03: signal interrupts blocking epoll_wait with -EINTR ── */

static void test_epoll_wait_signal_eintr(void) {
    install_ep_handler(SIGUSR1, ep_sig_handler);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        install_ep_handler(SIGUSR1, ep_sig_handler);
        long efd = sc2(SYS_EVENTFD2, 0, 0);
        if (efd < 0) sc1(SYS_EXIT, 99);
        long epfd = sc1(SYS_EPOLL_CREATE1, 0);
        if (epfd < 0) sc1(SYS_EXIT, 98);
        struct epoll_event ev = { .events = EPOLLIN, .data = 1 };
        sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, efd, (long)&ev);

        struct epoll_event out;
        /* Long timeout — only the signal should wake us. */
        long n = sc4(SYS_EPOLL_WAIT, epfd, (long)&out, 1, 5000);
        sc1(SYS_EXIT, n == -EINTR ? 7 : 0);
        __builtin_unreachable();
    }
    struct k_timespec delay = { .tv_sec = 0, .tv_nsec = 50000000 };
    sc2(SYS_NANOSLEEP, (long)&delay, 0);
    sc2(SYS_KILL, pid, SIGUSR1);

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("signal: epoll_wait returned -EINTR", (ws >> 8) & 0xFF, 7);
}

/* ── 04: epoll_wait with finite timeout returns 0 when no event fires ── */

static void test_epoll_wait_timeout(void) {
    long efd = sc2(SYS_EVENTFD2, 0, 0);
    if (efd < 0) { check("eventfd2 ok", 0); return; }
    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    if (epfd < 0) { sc1(SYS_CLOSE, efd); check("epoll_create1 ok", 0); return; }
    struct epoll_event ev = { .events = EPOLLIN, .data = 0 };
    sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, efd, (long)&ev);

    struct k_timespec t0, t1;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t0);
    struct epoll_event out;
    long n = sc4(SYS_EPOLL_WAIT, epfd, (long)&out, 1, 100 /* 100ms */);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
    long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

    check_val("timeout: epoll_wait returned 0", n, 0);
    check("timeout: blocked >= 50ms", elapsed_ms >= 50);
    check("timeout: returned within 1s", elapsed_ms < 1000);

    sc1(SYS_CLOSE, epfd);
    sc1(SYS_CLOSE, efd);
}

TEST("epoll/wait_eventfd_wakeup", test_epoll_wait_eventfd_wakeup);
TEST("epoll/wait_pipe_wakeup",    test_epoll_wait_pipe_wakeup);
TEST("epoll/wait_signal_eintr",   test_epoll_wait_signal_eintr);
TEST("epoll/wait_timeout",        test_epoll_wait_timeout);
