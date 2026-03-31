/* LTP connect01, connect02 — ported to ktest */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

/* ── connect01: error cases ── */

/* EBADF for invalid fd */
static void test_connect01_ebadf(void) {
    puts("\n[ltp/connect01-ebadf]\n");

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    sa.addr = 0x0100007F;
    sa.port = 0x5000;  /* arbitrary */

    long r = sc3(SYS_CONNECT, -1, (long)&sa, 16);
    check_val("connect(-1) EBADF", r, -EBADF);

    r = sc3(SYS_CONNECT, 999, (long)&sa, 16);
    check_val("connect(999) EBADF", r, -EBADF);
}

/* ENOTSOCK for non-socket fd */
static void test_connect01_enotsock(void) {
    puts("\n[ltp/connect01-enotsock]\n");

    long fd = sc3(SYS_OPEN, (long)"/dev/null", O_RDONLY, 0);
    check("open", fd >= 0);
    if (fd < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    sa.addr = 0x0100007F;
    sa.port = 0x5000;

    long r = sc3(SYS_CONNECT, fd, (long)&sa, 16);
    check_val("connect file ENOTSOCK", r, -ENOTSOCK);

    sc1(SYS_CLOSE, fd);
}

/* EAFNOSUPPORT for bad address family */
static void test_connect01_eafnosupport(void) {
    puts("\n[ltp/connect01-eafnosupport]\n");

    long sfd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("socket", sfd >= 0);
    if (sfd < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = 255;  /* invalid AF */
    sa.port = 0x5000;

    long r = sc3(SYS_CONNECT, sfd, (long)&sa, 16);
    /* EAFNOSUPPORT or EINVAL both acceptable */
    check("connect bad AF fails", r < 0);

    sc1(SYS_CLOSE, sfd);
}

/* ECONNREFUSED for connect to port with no listener */
static void test_connect01_econnrefused(void) {
    puts("\n[ltp/connect01-econnrefused]\n");

    long sfd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("socket", sfd >= 0);
    if (sfd < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    sa.addr = 0x0100007F;  /* 127.0.0.1 */
    sa.port = 0xFEFE;      /* unlikely to be listening */

    long r = sc3(SYS_CONNECT, sfd, (long)&sa, 16);
    check_val("connect refused ECONNREFUSED", r, -ECONNREFUSED);

    sc1(SYS_CLOSE, sfd);
}

/* ── connect02: successful loopback connect ── */

static void test_connect02(void) {
    puts("\n[ltp/connect02]\n");

    /* Set up listener */
    long lfd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("listen socket", lfd >= 0);
    if (lfd < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    sa.addr = 0x0100007F;

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
        long cfd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
        struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } csa;
        for (int i = 0; i < 16; i++) ((char *)&csa)[i] = 0;
        csa.family = AF_INET;
        csa.port = port;
        csa.addr = 0x0100007F;

        long cr = sc3(SYS_CONNECT, cfd, (long)&csa, 16);

        /* Send test data */
        if (cr == 0)
            sc3(SYS_WRITE, cfd, (long)"HELLO", 5);

        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT_GROUP, (cr == 0) ? 0 : 1);
        __builtin_unreachable();
    }

    /* Parent: accept and verify */
    long afd = sc3(SYS_ACCEPT, lfd, 0, 0);
    if (afd >= 0) {
        char buf[8] = {0};
        long n = sc3(SYS_READ, afd, (long)buf, 5);
        check_val("read 5 bytes", n, 5);
        check("data matches", buf[0] == 'H' && buf[4] == 'O');
        sc1(SYS_CLOSE, afd);
    } else {
        fail("accept", "loopback accept failed");
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));
    check_val("child connect ok", (long)WEXITSTATUS(wstatus), 0);

    sc1(SYS_CLOSE, lfd);
}

/* ── connect: EISCONN — connect on already-connected socket ── */

static void test_connect_eisconn(void) {
    puts("\n[ltp/connect-eisconn]\n");

    long lfd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("listen socket", lfd >= 0);
    if (lfd < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    sa.addr = 0x0100007F;

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
        long cfd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
        struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } csa;
        for (int i = 0; i < 16; i++) ((char *)&csa)[i] = 0;
        csa.family = AF_INET;
        csa.port = port;
        csa.addr = 0x0100007F;

        long cr = sc3(SYS_CONNECT, cfd, (long)&csa, 16);
        if (cr != 0) {
            sc1(SYS_CLOSE, cfd);
            sc1(SYS_EXIT_GROUP, 2);
            __builtin_unreachable();
        }

        /* Connect again — should get EISCONN */
        long cr2 = sc3(SYS_CONNECT, cfd, (long)&csa, 16);
        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT_GROUP, (cr2 == -EISCONN) ? 0 : 1);
        __builtin_unreachable();
    }

    long afd = sc3(SYS_ACCEPT, lfd, 0, 0);
    if (afd >= 0) sc1(SYS_CLOSE, afd);

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));
    check_val("connect EISCONN", (long)WEXITSTATUS(wstatus), 0);

    sc1(SYS_CLOSE, lfd);
}

TEST("ltp/connect01-ebadf",         test_connect01_ebadf);
TEST("ltp/connect01-enotsock",      test_connect01_enotsock);
TEST("ltp/connect01-eafnosupport",  test_connect01_eafnosupport);
TEST("ltp/connect01-econnrefused",  test_connect01_econnrefused);
TEST("ltp/connect02",               test_connect02);
TEST("ltp/connect-eisconn",         test_connect_eisconn);
