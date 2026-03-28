/* test: Unix socket poll — POLLIN, POLLOUT, POLLHUP */
#include "ktest.h"

struct test_pollfd { int fd; short events; short revents; };
#define T_POLLIN  0x0001
#define T_POLLOUT 0x0004
#define T_POLLHUP 0x0010

/* socketpair, write one end, poll other → POLLIN */
static void test_usock_poll_read(void) {
    puts("\n[unix-poll: POLLIN]\n");

    int sv[2] = { -1, -1 };
    long r = sc4(SYS_SOCKETPAIR, AF_UNIX, SOCK_STREAM, 0, (long)sv);
    check_val("socketpair", r, 0);
    if (r != 0) return;

    sc3(SYS_WRITE, sv[0], (long)"X", 1);

    struct test_pollfd pfd = { sv[1], T_POLLIN, 0 };
    r = sc3(SYS_POLL, (long)&pfd, 1, 100);
    check("poll returns ready", r >= 1);
    check("POLLIN set", (pfd.revents & T_POLLIN) != 0);

    sc1(SYS_CLOSE, sv[0]);
    sc1(SYS_CLOSE, sv[1]);
}

/* socketpair, poll write end → POLLOUT (buffer not full) */
static void test_usock_poll_write(void) {
    puts("\n[unix-poll: POLLOUT]\n");

    int sv[2] = { -1, -1 };
    long r = sc4(SYS_SOCKETPAIR, AF_UNIX, SOCK_STREAM, 0, (long)sv);
    check_val("socketpair", r, 0);
    if (r != 0) return;

    struct test_pollfd pfd = { sv[0], T_POLLOUT, 0 };
    r = sc3(SYS_POLL, (long)&pfd, 1, 100);
    check("poll returns ready", r >= 1);
    check("POLLOUT set", (pfd.revents & T_POLLOUT) != 0);

    sc1(SYS_CLOSE, sv[0]);
    sc1(SYS_CLOSE, sv[1]);
}

/* socketpair, close one end, poll other → POLLHUP */
static void test_usock_poll_hangup(void) {
    puts("\n[unix-poll: POLLHUP]\n");

    int sv[2] = { -1, -1 };
    long r = sc4(SYS_SOCKETPAIR, AF_UNIX, SOCK_STREAM, 0, (long)sv);
    check_val("socketpair", r, 0);
    if (r != 0) return;

    sc1(SYS_CLOSE, sv[0]); /* close one end */

    struct test_pollfd pfd = { sv[1], T_POLLIN, 0 };
    r = sc3(SYS_POLL, (long)&pfd, 1, 100);
    check("poll returns ready", r >= 1);
    check("POLLHUP set", (pfd.revents & T_POLLHUP) != 0);

    sc1(SYS_CLOSE, sv[1]);
}

/* socketpair, sendmsg on one end, recv on other */
static void test_usock_sendmsg(void) {
    puts("\n[unix-poll: sendmsg]\n");

    struct k_iovec { const void *base; uint64_t len; };
    struct k_msghdr {
        void *name; uint32_t namelen; uint32_t _p0;
        struct k_iovec *iov; uint64_t iovlen;
        void *control; uint64_t controllen;
        int flags; int _p1;
    };

    int sv[2] = { -1, -1 };
    long r = sc4(SYS_SOCKETPAIR, AF_UNIX, SOCK_STREAM, 0, (long)sv);
    check_val("socketpair", r, 0);
    if (r != 0) return;

    const char *msg = "sendmsg-test";
    struct k_iovec siov = { msg, 12 };
    struct k_msghdr shdr = { 0, 0, 0, &siov, 1, 0, 0, 0, 0 };
    r = sc3(SYS_SENDMSG, sv[0], (long)&shdr, 0);
    check_val("sendmsg returns 12", r, 12);

    char rbuf[16] = {0};
    r = sc3(SYS_READ, sv[1], (long)rbuf, 16);
    check_val("recv returns 12", r, 12);
    check("data 's'", rbuf[0] == 's');
    check("data 't'", rbuf[11] == 't');

    sc1(SYS_CLOSE, sv[0]);
    sc1(SYS_CLOSE, sv[1]);
}

TEST("unix-poll-read", test_usock_poll_read);
TEST("unix-poll-write", test_usock_poll_write);
TEST("unix-poll-hangup", test_usock_poll_hangup);
TEST("unix-sendmsg", test_usock_sendmsg);
