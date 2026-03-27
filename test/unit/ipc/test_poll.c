#include "ktest.h"

/* ── poll timeout=-1 ─────────────────────────── */

struct test_pollfd { int fd; short events; short revents; };
#define T_POLLIN  0x0001

static void test_poll_infinite(void) {
    puts("\n[poll timeout=-1]\n");

    int pipefd[2];
    long r = sc2(SYS_PIPE2, (long)pipefd, 0);
    check_val("pipe2", r, 0);
    if (r < 0) return;

    /* Write data so poll returns immediately */
    sc3(SYS_WRITE, pipefd[1], (long)"x", 1);

    struct test_pollfd pfd;
    pfd.fd = pipefd[0];
    pfd.events = T_POLLIN;
    pfd.revents = 0;
    r = sc3(SYS_POLL, (long)&pfd, 1, -1);
    check("poll(-1) returns ready", r >= 1);
    check("POLLIN set", (pfd.revents & T_POLLIN) != 0);

    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

TEST("poll timeout=-1", test_poll_infinite);
