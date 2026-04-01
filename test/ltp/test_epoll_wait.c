/* LTP epoll_wait01-07, epoll_pwait01-02, epoll_pwait04-06 — ported to ktest */
#include "ktest.h"

#define EPOLL_CLOEXEC   02000000
#define EPOLLONESHOT    (1U << 30)
#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)


/* ── epoll_wait01: EPOLLOUT on empty pipe write end ── */

static void test_epoll_wait01_out(void) {
    puts("\n[ltp/epoll_wait01-out]\n");

    int pipefd[2];
    sc1(SYS_PIPE, (long)pipefd);

    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) { sc1(SYS_CLOSE, pipefd[0]); sc1(SYS_CLOSE, pipefd[1]); return; }

    struct epoll_event ev;
    ev.events = EPOLLOUT;
    ev.data = (uint64_t)pipefd[1];
    sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, pipefd[1], (long)&ev);

    struct epoll_event ret;
    long r = sc4(SYS_EPOLL_WAIT, epfd, (long)&ret, 1, 0);
    check_val("epoll_wait EPOLLOUT returns 1", r, 1);
    if (r == 1) {
        check_val("fd matches", (long)ret.data, pipefd[1]);
        check("events EPOLLOUT", (ret.events & EPOLLOUT) != 0);
    }

    sc1(SYS_CLOSE, epfd);
    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

/* ── epoll_wait01: EPOLLIN on pipe with data ── */

static void test_epoll_wait01_in(void) {
    puts("\n[ltp/epoll_wait01-in]\n");

    int pipefd[2];
    sc1(SYS_PIPE, (long)pipefd);

    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) { sc1(SYS_CLOSE, pipefd[0]); sc1(SYS_CLOSE, pipefd[1]); return; }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data = (uint64_t)pipefd[0];
    sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, pipefd[0], (long)&ev);

    sc3(SYS_WRITE, pipefd[1], (long)"hello", 5);

    struct epoll_event ret;
    long r = sc4(SYS_EPOLL_WAIT, epfd, (long)&ret, 1, 0);
    check_val("epoll_wait EPOLLIN returns 1", r, 1);
    if (r == 1) {
        check_val("fd matches", (long)ret.data, pipefd[0]);
        check("events EPOLLIN", (ret.events & EPOLLIN) != 0);
    }

    char buf[8];
    sc3(SYS_READ, pipefd[0], (long)buf, 5);

    sc1(SYS_CLOSE, epfd);
    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

/* ── epoll_wait03: error cases ── */

static void test_epoll_wait03_ebadf(void) {
    puts("\n[ltp/epoll_wait03-ebadf]\n");
    struct epoll_event ev;
    long r = sc4(SYS_EPOLL_WAIT, -1, (long)&ev, 1, 0);
    check_val("epoll_wait bad fd EBADF", r, -EBADF);
}

static void test_epoll_wait03_einval_notepoll(void) {
    puts("\n[ltp/epoll_wait03-einval-notepoll]\n");
    int pipefd[2];
    sc1(SYS_PIPE, (long)pipefd);
    struct epoll_event ev;
    long r = sc4(SYS_EPOLL_WAIT, pipefd[0], (long)&ev, 1, 0);
    check_val("epoll_wait on pipe EINVAL", r, -EINVAL);
    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

static void test_epoll_wait03_einval_maxevents(void) {
    puts("\n[ltp/epoll_wait03-einval-maxevents]\n");

    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) return;

    struct epoll_event ev;
    long r = sc4(SYS_EPOLL_WAIT, epfd, (long)&ev, -1, 0);
    check_val("maxevents < 0 EINVAL", r, -EINVAL);

    r = sc4(SYS_EPOLL_WAIT, epfd, (long)&ev, 0, 0);
    check_val("maxevents == 0 EINVAL", r, -EINVAL);

    sc1(SYS_CLOSE, epfd);
}

/* ── epoll_wait04: timeout=0 returns immediately ── */

static void test_epoll_wait04(void) {
    puts("\n[ltp/epoll_wait04]\n");

    int pipefd[2];
    sc1(SYS_PIPE, (long)pipefd);

    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) { sc1(SYS_CLOSE, pipefd[0]); sc1(SYS_CLOSE, pipefd[1]); return; }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data = (uint64_t)pipefd[0];
    sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, pipefd[0], (long)&ev);

    /* No data written — timeout=0 should return 0 immediately */
    struct epoll_event ret;
    long r = sc4(SYS_EPOLL_WAIT, epfd, (long)&ret, 1, 0);
    check_val("timeout=0 no data returns 0", r, 0);

    sc1(SYS_CLOSE, epfd);
    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

/* ── epoll_wait06: edge-triggered (EPOLLET) ── */

static void test_epoll_wait06(void) {
    puts("\n[ltp/epoll_wait06]\n");

    int pipefd[2];
    /* pipe2 with O_NONBLOCK */
    long r = sc2(SYS_PIPE2, (long)pipefd, O_NONBLOCK);
    check_val("pipe2 nonblock", r, 0);
    if (r != 0) return;

    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) { sc1(SYS_CLOSE, pipefd[0]); sc1(SYS_CLOSE, pipefd[1]); return; }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data = (uint64_t)pipefd[0];
    sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, pipefd[0], (long)&ev);

    /* Write data */
    char buf[64];
    for (int i = 0; i < 64; i++) buf[i] = 'a';
    sc3(SYS_WRITE, pipefd[1], (long)buf, 64);

    /* First wait: should get EPOLLIN */
    struct epoll_event ret;
    r = sc4(SYS_EPOLL_WAIT, epfd, (long)&ret, 1, 0);
    check_val("ET first wait returns 1", r, 1);

    /* Read only half */
    sc3(SYS_READ, pipefd[0], (long)buf, 32);

    /* Second wait with ET: should return 0 (no new event, still data left) */
    r = sc4(SYS_EPOLL_WAIT, epfd, (long)&ret, 1, 0);
    check_val("ET second wait (data left) returns 0", r, 0);

    /* Read remaining */
    sc3(SYS_READ, pipefd[0], (long)buf, 32);

    sc1(SYS_CLOSE, epfd);
    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

/* ── epoll_wait07: EPOLLONESHOT ── */

static void test_epoll_wait07(void) {
    puts("\n[ltp/epoll_wait07]\n");

    int pipefd[2];
    sc1(SYS_PIPE, (long)pipefd);

    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) { sc1(SYS_CLOSE, pipefd[0]); sc1(SYS_CLOSE, pipefd[1]); return; }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLONESHOT;
    ev.data = (uint64_t)pipefd[0];
    sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, pipefd[0], (long)&ev);

    /* Write and wait — first time should fire */
    char c = 'x';
    sc3(SYS_WRITE, pipefd[1], (long)&c, 1);

    struct epoll_event ret;
    long r = sc4(SYS_EPOLL_WAIT, epfd, (long)&ret, 1, 0);
    check_val("oneshot first wait returns 1", r, 1);

    /* Read the byte */
    sc3(SYS_READ, pipefd[0], (long)&c, 1);
    r = sc4(SYS_EPOLL_WAIT, epfd, (long)&ret, 1, 0);
    check_val("oneshot cleared returns 0", r, 0);

    /* Write again — should not fire (oneshot consumed) */
    sc3(SYS_WRITE, pipefd[1], (long)&c, 1);
    r = sc4(SYS_EPOLL_WAIT, epfd, (long)&ret, 1, 0);
    check_val("oneshot second write returns 0", r, 0);

    char buf[4];
    sc3(SYS_READ, pipefd[0], (long)buf, 1);

    sc1(SYS_CLOSE, epfd);
    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

/* ── epoll_pwait01: signal blocked by sigmask ── */

static void test_epoll_pwait01(void) {
    puts("\n[ltp/epoll_pwait01]\n");

    int pipefd[2];
    sc1(SYS_PIPE, (long)pipefd);

    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) { sc1(SYS_CLOSE, pipefd[0]); sc1(SYS_CLOSE, pipefd[1]); return; }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data = (uint64_t)pipefd[0];
    sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, pipefd[0], (long)&ev);

    /* Write data so fd is ready */
    sc3(SYS_WRITE, pipefd[1], (long)"x", 1);

    /* Block all signals in sigmask */
    uint64_t sigmask[2];
    sigmask[0] = ~0ULL;
    sigmask[1] = ~0ULL;

    struct epoll_event ret;
    long r = sc6(SYS_EPOLL_PWAIT, epfd, (long)&ret, 1, 0, (long)sigmask, 8);
    check_val("epoll_pwait with sigmask returns 1", r, 1);

    char buf[4];
    sc3(SYS_READ, pipefd[0], (long)buf, 1);

    sc1(SYS_CLOSE, epfd);
    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

/* ── epoll_pwait02: data ready returns immediately ── */

static void test_epoll_pwait02(void) {
    puts("\n[ltp/epoll_pwait02]\n");

    int pipefd[2];
    sc1(SYS_PIPE, (long)pipefd);

    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) { sc1(SYS_CLOSE, pipefd[0]); sc1(SYS_CLOSE, pipefd[1]); return; }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data = (uint64_t)pipefd[0];
    sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, pipefd[0], (long)&ev);

    sc3(SYS_WRITE, pipefd[1], (long)"w", 1);

    struct epoll_event ret;
    long r = sc6(SYS_EPOLL_PWAIT, epfd, (long)&ret, 1, 0, 0, 0);
    check_val("epoll_pwait data ready returns 1", r, 1);

    char buf[4];
    sc3(SYS_READ, pipefd[0], (long)buf, 1);

    sc1(SYS_CLOSE, epfd);
    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

/* ── epoll_pwait05: EINVAL for invalid timespec (epoll_pwait2) ── */

static void test_epoll_pwait05(void) {
    puts("\n[ltp/epoll_pwait05]\n");

    int pipefd[2];
    sc1(SYS_PIPE, (long)pipefd);

    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) { sc1(SYS_CLOSE, pipefd[0]); sc1(SYS_CLOSE, pipefd[1]); return; }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data = (uint64_t)pipefd[0];
    sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, pipefd[0], (long)&ev);

    sc3(SYS_WRITE, pipefd[1], (long)"w", 1);

    /* epoll_pwait2 with negative tv_sec */
    struct { long tv_sec; long tv_nsec; } ts;
    ts.tv_sec = -1;
    ts.tv_nsec = 0;
    struct epoll_event ret;
    long r = sc6(SYS_EPOLL_PWAIT2, epfd, (long)&ret, 1, (long)&ts, 0, 0);
    check_val("epoll_pwait2 tv_sec<0 EINVAL", r, -EINVAL);

    /* negative tv_nsec */
    ts.tv_sec = 0;
    ts.tv_nsec = -1;
    r = sc6(SYS_EPOLL_PWAIT2, epfd, (long)&ret, 1, (long)&ts, 0, 0);
    check_val("epoll_pwait2 tv_nsec<0 EINVAL", r, -EINVAL);

    /* tv_nsec >= 1e9 */
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000000L;
    r = sc6(SYS_EPOLL_PWAIT2, epfd, (long)&ret, 1, (long)&ts, 0, 0);
    check_val("epoll_pwait2 tv_nsec>=1e9 EINVAL", r, -EINVAL);

    char buf[4];
    sc3(SYS_READ, pipefd[0], (long)buf, 1);

    sc1(SYS_CLOSE, epfd);
    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

TEST("ltp/epoll_wait01-out",              test_epoll_wait01_out);
TEST("ltp/epoll_wait01-in",               test_epoll_wait01_in);
TEST("ltp/epoll_wait03-ebadf",            test_epoll_wait03_ebadf);
TEST("ltp/epoll_wait03-einval-notepoll",  test_epoll_wait03_einval_notepoll);
TEST("ltp/epoll_wait03-einval-maxevents", test_epoll_wait03_einval_maxevents);
TEST("ltp/epoll_wait04",                  test_epoll_wait04);
TEST("ltp/epoll_wait06",                  test_epoll_wait06);
TEST("ltp/epoll_wait07",                  test_epoll_wait07);
TEST("ltp/epoll_pwait01",                 test_epoll_pwait01);
TEST("ltp/epoll_pwait02",                 test_epoll_pwait02);
TEST("ltp/epoll_pwait05",                 test_epoll_pwait05);
