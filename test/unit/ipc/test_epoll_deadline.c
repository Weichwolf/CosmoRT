#include "ktest.h"

/* ── Epoll/poll deadline: TSC-based timeout precision ── */

struct edl_epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

#define T_EPOLLIN       0x001
#define T_EPOLLOUT      0x004
#define T_EPOLL_CTL_ADD 1
#define T_POLLIN        0x0001
#define T_POLLOUT       0x0004

/* Helper: get monotonic time in ms via clock_gettime */
static uint64_t now_ms(void) {
    struct k_timespec ts;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void test_epoll_timeout_zero(void) {
    puts("\n[epoll_deadline]\n");
    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) return;
    /* epoll_wait with timeout=0 on empty set → immediate return 0 */
    struct edl_epoll_event out;
    long r = sc4(SYS_EPOLL_WAIT, epfd, (long)&out, 1, 0);
    check_val("epoll_wait(0) = 0", r, 0);
    sc1(SYS_CLOSE, epfd);
}

static void test_epoll_timeout_short(void) {
    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll short create", epfd >= 0);
    if (epfd < 0) return;
    /* Add a pipe read end (won't have data) */
    int pfd[2];
    long r = sc2(SYS_PIPE2, (long)pfd, 0);
    check_val("pipe2", r, 0);
    if (r < 0) { sc1(SYS_CLOSE, epfd); return; }
    struct edl_epoll_event ev = { T_EPOLLIN, 1 };
    sc4(SYS_EPOLL_CTL, epfd, T_EPOLL_CTL_ADD, pfd[0], (long)&ev);
    uint64_t t0 = now_ms();
    struct edl_epoll_event out;
    r = sc4(SYS_EPOLL_WAIT, epfd, (long)&out, 1, 20); /* 20ms timeout */
    uint64_t elapsed = now_ms() - t0;
    check_val("epoll_wait(20) = 0", r, 0);
    check("elapsed >= 15ms", elapsed >= 15); /* allow 5ms jitter */
    check("elapsed < 200ms", elapsed < 200);
    sc1(SYS_CLOSE, pfd[0]);
    sc1(SYS_CLOSE, pfd[1]);
    sc1(SYS_CLOSE, epfd);
}

static void test_poll_timeout_zero(void) {
    int pfd[2];
    long r = sc2(SYS_PIPE2, (long)pfd, 0);
    check_val("poll0 pipe2", r, 0);
    if (r < 0) return;
    struct { int fd; short events; short revents; } kpfd;
    kpfd.fd = pfd[0]; kpfd.events = T_POLLIN; kpfd.revents = 0;
    r = sc3(SYS_POLL, (long)&kpfd, 1, 0);
    check_val("poll(0) = 0", r, 0);
    sc1(SYS_CLOSE, pfd[0]);
    sc1(SYS_CLOSE, pfd[1]);
}

static void test_poll_timeout_short(void) {
    int pfd[2];
    long r = sc2(SYS_PIPE2, (long)pfd, 0);
    check_val("pollshort pipe2", r, 0);
    if (r < 0) return;
    struct { int fd; short events; short revents; } kpfd;
    kpfd.fd = pfd[0]; kpfd.events = T_POLLIN; kpfd.revents = 0;
    uint64_t t0 = now_ms();
    r = sc3(SYS_POLL, (long)&kpfd, 1, 20);
    uint64_t elapsed = now_ms() - t0;
    check_val("poll(20) = 0", r, 0);
    check("poll elapsed >= 15ms", elapsed >= 15);
    check("poll elapsed < 200ms", elapsed < 200);
    sc1(SYS_CLOSE, pfd[0]);
    sc1(SYS_CLOSE, pfd[1]);
}

static void test_epoll_immediate_ready(void) {
    /* Pipe with data: epoll_wait should return immediately */
    int pfd[2];
    long r = sc2(SYS_PIPE2, (long)pfd, 0);
    check_val("imm pipe2", r, 0);
    if (r < 0) return;
    sc3(SYS_WRITE, pfd[1], (long)"x", 1);
    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    struct edl_epoll_event ev = { T_EPOLLIN, 42 };
    sc4(SYS_EPOLL_CTL, epfd, T_EPOLL_CTL_ADD, pfd[0], (long)&ev);
    uint64_t t0 = now_ms();
    struct edl_epoll_event out;
    r = sc4(SYS_EPOLL_WAIT, epfd, (long)&out, 1, 1000);
    uint64_t elapsed = now_ms() - t0;
    check("epoll immediate = 1", r == 1);
    check("immediate < 50ms", elapsed < 50);
    sc1(SYS_CLOSE, pfd[0]);
    sc1(SYS_CLOSE, pfd[1]);
    sc1(SYS_CLOSE, epfd);
}

static void test_poll_immediate_ready(void) {
    int pfd[2];
    long r = sc2(SYS_PIPE2, (long)pfd, 0);
    check_val("pollimm pipe2", r, 0);
    if (r < 0) return;
    sc3(SYS_WRITE, pfd[1], (long)"y", 1);
    struct { int fd; short events; short revents; } kpfd;
    kpfd.fd = pfd[0]; kpfd.events = T_POLLIN; kpfd.revents = 0;
    uint64_t t0 = now_ms();
    r = sc3(SYS_POLL, (long)&kpfd, 1, 1000);
    uint64_t elapsed = now_ms() - t0;
    check("poll immediate >= 1", r >= 1);
    check("poll immediate < 50ms", elapsed < 50);
    sc1(SYS_CLOSE, pfd[0]);
    sc1(SYS_CLOSE, pfd[1]);
}

static void test_epoll_multiple_fds(void) {
    int p1[2], p2[2];
    long r = sc2(SYS_PIPE2, (long)p1, 0);
    check_val("multi p1", r, 0);
    r = sc2(SYS_PIPE2, (long)p2, 0);
    check_val("multi p2", r, 0);
    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    struct edl_epoll_event ev1 = { T_EPOLLIN, 1 };
    struct edl_epoll_event ev2 = { T_EPOLLIN, 2 };
    sc4(SYS_EPOLL_CTL, epfd, T_EPOLL_CTL_ADD, p1[0], (long)&ev1);
    sc4(SYS_EPOLL_CTL, epfd, T_EPOLL_CTL_ADD, p2[0], (long)&ev2);
    /* Write to p2 only */
    sc3(SYS_WRITE, p2[1], (long)"z", 1);
    struct edl_epoll_event out[2];
    r = sc4(SYS_EPOLL_WAIT, epfd, (long)out, 2, 50);
    check("multi returns >= 1", r >= 1);
    sc1(SYS_CLOSE, p1[0]); sc1(SYS_CLOSE, p1[1]);
    sc1(SYS_CLOSE, p2[0]); sc1(SYS_CLOSE, p2[1]);
    sc1(SYS_CLOSE, epfd);
}

static void test_poll_multiple_fds(void) {
    int p1[2], p2[2];
    long r = sc2(SYS_PIPE2, (long)p1, 0);
    check_val("pmulti p1", r, 0);
    r = sc2(SYS_PIPE2, (long)p2, 0);
    check_val("pmulti p2", r, 0);
    sc3(SYS_WRITE, p1[1], (long)"a", 1);
    sc3(SYS_WRITE, p2[1], (long)"b", 1);
    struct { int fd; short events; short revents; } kpfds[2];
    kpfds[0].fd = p1[0]; kpfds[0].events = T_POLLIN; kpfds[0].revents = 0;
    kpfds[1].fd = p2[0]; kpfds[1].events = T_POLLIN; kpfds[1].revents = 0;
    r = sc3(SYS_POLL, (long)kpfds, 2, 50);
    check_val("poll multi = 2", r, 2);
    sc1(SYS_CLOSE, p1[0]); sc1(SYS_CLOSE, p1[1]);
    sc1(SYS_CLOSE, p2[0]); sc1(SYS_CLOSE, p2[1]);
}

static void test_epoll_timeout_accuracy(void) {
    /* Test that 50ms timeout is reasonably accurate */
    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    int pfd[2];
    sc2(SYS_PIPE2, (long)pfd, 0);
    struct edl_epoll_event ev = { T_EPOLLIN, 1 };
    sc4(SYS_EPOLL_CTL, epfd, T_EPOLL_CTL_ADD, pfd[0], (long)&ev);
    uint64_t t0 = now_ms();
    struct edl_epoll_event out;
    long r = sc4(SYS_EPOLL_WAIT, epfd, (long)&out, 1, 50);
    uint64_t elapsed = now_ms() - t0;
    check_val("accuracy = 0", r, 0);
    check("accuracy >= 40ms", elapsed >= 40);
    check("accuracy < 300ms", elapsed < 300);
    sc1(SYS_CLOSE, pfd[0]);
    sc1(SYS_CLOSE, pfd[1]);
    sc1(SYS_CLOSE, epfd);
}

static void test_poll_writable(void) {
    int pfd[2];
    long r = sc2(SYS_PIPE2, (long)pfd, 0);
    check_val("writable pipe2", r, 0);
    if (r < 0) return;
    struct { int fd; short events; short revents; } kpfd;
    kpfd.fd = pfd[1]; kpfd.events = T_POLLOUT; kpfd.revents = 0;
    r = sc3(SYS_POLL, (long)&kpfd, 1, 0);
    check("poll writable >= 1", r >= 1);
    check("POLLOUT set", (kpfd.revents & T_POLLOUT) != 0);
    sc1(SYS_CLOSE, pfd[0]);
    sc1(SYS_CLOSE, pfd[1]);
}

TEST("epoll_dl_zero", test_epoll_timeout_zero);
TEST("epoll_dl_short", test_epoll_timeout_short);
TEST("poll_dl_zero", test_poll_timeout_zero);
TEST("poll_dl_short", test_poll_timeout_short);
TEST("epoll_dl_imm", test_epoll_immediate_ready);
TEST("poll_dl_imm", test_poll_immediate_ready);
TEST("epoll_dl_multi", test_epoll_multiple_fds);
TEST("poll_dl_multi", test_poll_multiple_fds);
TEST("epoll_dl_accur", test_epoll_timeout_accuracy);
TEST("poll_dl_writ", test_poll_writable);
