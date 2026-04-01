/* LTP epoll_ctl01-05 — ported to ktest */
#include "ktest.h"


/* ── epoll_ctl01: basic ADD/MOD/DEL with pipe ── */

static void test_epoll_ctl01(void) {
    puts("\n[ltp/epoll_ctl01]\n");

    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) return;

    int pipefd[2];
    long r = sc1(SYS_PIPE, (long)pipefd);
    check_val("pipe", r, 0);
    if (r != 0) { sc1(SYS_CLOSE, epfd); return; }

    /* ADD read end with EPOLLIN */
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data = (uint64_t)pipefd[0];
    r = sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, pipefd[0], (long)&ev);
    check_val("EPOLL_CTL_ADD read", r, 0);

    /* ADD write end with EPOLLIN (will get EPOLLOUT later via MOD) */
    ev.events = EPOLLIN;
    ev.data = (uint64_t)pipefd[1];
    r = sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, pipefd[1], (long)&ev);
    check_val("EPOLL_CTL_ADD write", r, 0);

    /* Write some data so pipe read end is readable */
    sc3(SYS_WRITE, pipefd[1], (long)"test", 4);

    /* epoll_wait should report EPOLLIN on pipefd[0] */
    struct epoll_event out;
    r = sc4(SYS_EPOLL_WAIT, epfd, (long)&out, 1, 0);
    check("epoll_wait after ADD returns >= 1", r >= 1);

    /* Drain pipe */
    char buf[8];
    sc3(SYS_READ, pipefd[0], (long)buf, 4);

    /* MOD write end to EPOLLOUT */
    ev.events = EPOLLOUT;
    ev.data = (uint64_t)pipefd[1];
    r = sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_MOD, pipefd[1], (long)&ev);
    check_val("EPOLL_CTL_MOD", r, 0);

    /* Write data again, then wait — should see both EPOLLIN and EPOLLOUT */
    sc3(SYS_WRITE, pipefd[1], (long)"test", 4);
    struct epoll_event out2[2];
    r = sc4(SYS_EPOLL_WAIT, epfd, (long)out2, 2, 0);
    check("epoll_wait after MOD returns >= 1", r >= 1);

    /* Drain pipe */
    sc3(SYS_READ, pipefd[0], (long)buf, 4);

    /* DEL write end */
    r = sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_DEL, pipefd[1], 0);
    check_val("EPOLL_CTL_DEL write", r, 0);

    /* DEL read end */
    r = sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_DEL, pipefd[0], 0);
    check_val("EPOLL_CTL_DEL read", r, 0);

    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
    sc1(SYS_CLOSE, epfd);
}

/* ── epoll_ctl02: error cases ── */

static void test_epoll_ctl02_ebadf(void) {
    puts("\n[ltp/epoll_ctl02-ebadf]\n");

    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) return;

    int pipefd[2];
    sc1(SYS_PIPE, (long)pipefd);

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data = 0;

    /* EBADF: invalid epfd */
    long r = sc4(SYS_EPOLL_CTL, -1, EPOLL_CTL_ADD, pipefd[1], (long)&ev);
    check_val("invalid epfd EBADF", r, -EBADF);

    /* EBADF: invalid fd */
    r = sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, -1, (long)&ev);
    check_val("invalid fd EBADF", r, -EBADF);

    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
    sc1(SYS_CLOSE, epfd);
}

static void test_epoll_ctl02_einval(void) {
    puts("\n[ltp/epoll_ctl02-einval]\n");

    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) return;

    int pipefd[2];
    sc1(SYS_PIPE, (long)pipefd);

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data = 0;

    /* EINVAL: invalid op */
    long r = sc4(SYS_EPOLL_CTL, epfd, -1, pipefd[1], (long)&ev);
    check_val("invalid op EINVAL", r, -EINVAL);

    /* EINVAL: fd == epfd */
    r = sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, epfd, (long)&ev);
    check_val("fd==epfd EINVAL", r, -EINVAL);

    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
    sc1(SYS_CLOSE, epfd);
}

static void test_epoll_ctl02_eperm(void) {
    puts("\n[ltp/epoll_ctl02-eperm]\n");

    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) return;

    /* Open a regular file — doesn't support epoll */
    long fd = sc3(SYS_OPEN, (long)"/tmp", O_RDONLY | O_DIRECTORY, 0);
    check("open /tmp", fd >= 0);
    if (fd < 0) { sc1(SYS_CLOSE, epfd); return; }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data = 0;
    long r = sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, fd, (long)&ev);
    check_val("regular file EPERM", r, -EPERM);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_CLOSE, epfd);
}

static void test_epoll_ctl02_enoent(void) {
    puts("\n[ltp/epoll_ctl02-enoent]\n");

    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) return;

    int pipefd[2];
    sc1(SYS_PIPE, (long)pipefd);

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data = 0;

    /* ENOENT: DEL on unregistered fd */
    long r = sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_DEL, pipefd[1], (long)&ev);
    check_val("DEL unregistered ENOENT", r, -ENOENT);

    /* ENOENT: MOD on unregistered fd */
    r = sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_MOD, pipefd[1], (long)&ev);
    check_val("MOD unregistered ENOENT", r, -ENOENT);

    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
    sc1(SYS_CLOSE, epfd);
}

static void test_epoll_ctl02_eexist(void) {
    puts("\n[ltp/epoll_ctl02-eexist]\n");

    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) return;

    int pipefd[2];
    sc1(SYS_PIPE, (long)pipefd);

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data = (uint64_t)pipefd[0];

    /* ADD first time — OK */
    long r = sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, pipefd[0], (long)&ev);
    check_val("first ADD", r, 0);

    /* ADD same fd again — EEXIST */
    r = sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, pipefd[0], (long)&ev);
    check_val("duplicate ADD EEXIST", r, -EEXIST);

    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
    sc1(SYS_CLOSE, epfd);
}

/* ── epoll_ctl03: various event combinations succeed ── */

static void test_epoll_ctl03(void) {
    puts("\n[ltp/epoll_ctl03]\n");

    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) return;

    int pipefd[2];
    sc1(SYS_PIPE, (long)pipefd);

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data = (uint64_t)pipefd[0];
    long r = sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, pipefd[0], (long)&ev);
    check_val("initial ADD", r, 0);

    /* MOD with various event combos */
    uint32_t combos[] = {
        EPOLLIN, EPOLLOUT, EPOLLIN | EPOLLOUT,
        EPOLLERR, EPOLLHUP, EPOLLET,
        EPOLLIN | EPOLLET, EPOLLRDHUP,
    };
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        ev.events = combos[i];
        r = sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_MOD, pipefd[0], (long)&ev);
        if (r != 0) { ok = 0; break; }
    }
    check("MOD various event combos", ok);

    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
    sc1(SYS_CLOSE, epfd);
}

/* ── epoll_ctl04: max nesting depth = 5 ── */

static void test_epoll_ctl04(void) {
    puts("\n[ltp/epoll_ctl04]\n");

    int pipefd[2];
    sc1(SYS_PIPE, (long)pipefd);

    /* Build chain: pipe -> ep0 -> ep1 -> ep2 -> ep3 -> ep4 */
    long prev = pipefd[0];
    long epfds[6];
    struct epoll_event ev;
    ev.events = EPOLLIN;

    int i;
    for (i = 0; i < 5; i++) {
        epfds[i] = sc1(SYS_EPOLL_CREATE1, 0);
        check("epoll_create1 chain", epfds[i] >= 0);
        if (epfds[i] < 0) goto cleanup;

        ev.data = (uint64_t)prev;
        long r = sc4(SYS_EPOLL_CTL, epfds[i], EPOLL_CTL_ADD, prev, (long)&ev);
        check_val("chain ADD", r, 0);
        prev = epfds[i];
    }

    /* 6th level should fail with EINVAL or ELOOP */
    epfds[5] = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1 overflow", epfds[5] >= 0);
    if (epfds[5] >= 0) {
        ev.data = (uint64_t)prev;
        long r = sc4(SYS_EPOLL_CTL, epfds[5], EPOLL_CTL_ADD, prev, (long)&ev);
        check("nesting > 5 fails", r == -EINVAL || r == -ELOOP);
        sc1(SYS_CLOSE, epfds[5]);
    }

cleanup:
    for (int j = 0; j < i; j++)
        sc1(SYS_CLOSE, epfds[j]);
    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

/* ── epoll_ctl05: circular loop detection (ELOOP) ── */

static void test_epoll_ctl05(void) {
    puts("\n[ltp/epoll_ctl05]\n");

    int pipefd[2];
    sc1(SYS_PIPE, (long)pipefd);

    /* Build chain: pipe -> ep0 -> ep1 -> ep2 -> ep3 -> ep4 */
    long prev = pipefd[0];
    long epfds[5];
    struct epoll_event ev;
    ev.events = EPOLLIN;

    long origin = -1;
    int i;
    for (i = 0; i < 5; i++) {
        epfds[i] = sc1(SYS_EPOLL_CREATE1, 0);
        if (epfds[i] < 0) goto cleanup;

        if (i == 0) origin = epfds[0];

        ev.data = (uint64_t)prev;
        sc4(SYS_EPOLL_CTL, epfds[i], EPOLL_CTL_ADD, prev, (long)&ev);
        prev = epfds[i];
    }

    /* Remove pipe from origin to make room */
    ev.data = (uint64_t)pipefd[0];
    sc4(SYS_EPOLL_CTL, origin, EPOLL_CTL_DEL, pipefd[0], (long)&ev);

    /* Try to add last epfd into origin — circular → ELOOP */
    ev.data = (uint64_t)prev;
    long r = sc4(SYS_EPOLL_CTL, origin, EPOLL_CTL_ADD, prev, (long)&ev);
    check_val("circular ELOOP", r, -ELOOP);

cleanup:
    for (int j = 0; j < i; j++)
        sc1(SYS_CLOSE, epfds[j]);
    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

TEST("ltp/epoll_ctl01",          test_epoll_ctl01);
TEST("ltp/epoll_ctl02-ebadf",    test_epoll_ctl02_ebadf);
TEST("ltp/epoll_ctl02-einval",   test_epoll_ctl02_einval);
TEST("ltp/epoll_ctl02-eperm",    test_epoll_ctl02_eperm);
TEST("ltp/epoll_ctl02-enoent",   test_epoll_ctl02_enoent);
TEST("ltp/epoll_ctl02-eexist",   test_epoll_ctl02_eexist);
TEST("ltp/epoll_ctl03",          test_epoll_ctl03);
TEST("ltp/epoll_ctl04",          test_epoll_ctl04);
TEST("ltp/epoll_ctl05",          test_epoll_ctl05);
