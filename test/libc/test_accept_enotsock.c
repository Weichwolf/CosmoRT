/* test: accept() error codes — ENOTSOCK wrong errno + accept4 flags ignored */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define SOCK_CLOEXEC    0x80000
#define SOCK_NONBLOCK   0x800

/*
 * accept() on non-socket fd returns EBADF instead of ENOTSOCK.
 * Linux: valid fd that isn't a socket → ENOTSOCK.
 * CosmoRT: returns EBADF (wrong).
 */
static void test_accept_enotsock(void) {
    puts("\n[net/accept-enotsock]\n");

    long fd = sc3(SYS_OPEN, (long)"/dev/null", O_RDONLY, 0);
    check("open /dev/null", fd >= 0);
    if (fd < 0) return;

    long r = sc3(SYS_ACCEPT, fd, 0, 0);
    check_val("accept ENOTSOCK", r, -ENOTSOCK);

    sc1(SYS_CLOSE, fd);
}

/*
 * accept4() with SOCK_CLOEXEC|SOCK_NONBLOCK should set O_NONBLOCK on
 * the accepted fd. Currently flags are ignored.
 */
static void test_accept4_flags(void) {
    puts("\n[net/accept4-flags]\n");

    int lfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("listen socket", lfd >= 0);
    if (lfd < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    sa.port = 0;
    sa.addr = 0x0100007F; /* 127.0.0.1 */

    long r = sc3(SYS_BIND, lfd, (long)&sa, 16);
    check_val("bind", r, 0);
    if (r != 0) { sc1(SYS_CLOSE, lfd); return; }

    int addrlen = 16;
    sc3(SYS_GETSOCKNAME, lfd, (long)&sa, (long)&addrlen);
    uint16_t port = sa.port;

    r = sc2(SYS_LISTEN, lfd, 5);
    check_val("listen", r, 0);

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) { sc1(SYS_CLOSE, lfd); return; }

    if (pid == 0) {
        sc1(SYS_CLOSE, lfd);
        int cfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
        struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } csa;
        for (int i = 0; i < 16; i++) ((char *)&csa)[i] = 0;
        csa.family = AF_INET;
        csa.port = port;
        csa.addr = 0x0100007F;
        long cr = sc3(SYS_CONNECT, cfd, (long)&csa, 16);
        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT_GROUP, (cr == 0) ? 0 : 1);
        __builtin_unreachable();
    }

    /* accept4 with SOCK_CLOEXEC | SOCK_NONBLOCK */
    int afd = (int)sc4(SYS_ACCEPT4, lfd, 0, 0, SOCK_CLOEXEC | SOCK_NONBLOCK);

    /* Even if accept4 itself fails (loopback bug), test the flag path */
    if (afd >= 0) {
        long fl = sc2(SYS_FCNTL, afd, F_GETFL);
        check("accept4 O_NONBLOCK set", (fl & O_NONBLOCK) != 0);
        sc1(SYS_CLOSE, afd);
    } else {
        /* accept4 failed — still a bug, record it */
        fail("accept4 returned error", "loopback accept failed");
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));

    sc1(SYS_CLOSE, lfd);
}

TEST("net/accept-enotsock", test_accept_enotsock);
TEST("net/accept4-flags", test_accept4_flags);
