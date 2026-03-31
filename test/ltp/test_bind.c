/* LTP bind01-05 — ported to ktest */
#include "ktest.h"

/* ── bind01: basic bind to INADDR_ANY ── */

static void test_bind01(void) {
    puts("\n[ltp/bind01]\n");

    /* TCP */
    long sfd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("TCP socket", sfd >= 0);
    if (sfd < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    sa.port = 0;       /* kernel picks port */
    sa.addr = 0;       /* INADDR_ANY */

    long r = sc3(SYS_BIND, sfd, (long)&sa, 16);
    check_val("bind TCP INADDR_ANY", r, 0);

    /* Verify port was assigned */
    int addrlen = 16;
    r = sc3(SYS_GETSOCKNAME, sfd, (long)&sa, (long)&addrlen);
    check_val("getsockname", r, 0);
    check("port assigned", sa.port != 0);

    sc1(SYS_CLOSE, sfd);

    /* UDP */
    sfd = sc3(SYS_SOCKET, AF_INET, SOCK_DGRAM, 0);
    check("UDP socket", sfd >= 0);
    if (sfd < 0) return;

    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    r = sc3(SYS_BIND, sfd, (long)&sa, 16);
    check_val("bind UDP INADDR_ANY", r, 0);

    sc1(SYS_CLOSE, sfd);
}

/* ── bind02: bind to loopback 127.0.0.1 ── */

static void test_bind02(void) {
    puts("\n[ltp/bind02]\n");

    long sfd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("socket", sfd >= 0);
    if (sfd < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    sa.port = 0;
    sa.addr = 0x0100007F;  /* 127.0.0.1 */

    long r = sc3(SYS_BIND, sfd, (long)&sa, 16);
    check_val("bind loopback", r, 0);

    /* Verify address */
    int addrlen = 16;
    sc3(SYS_GETSOCKNAME, sfd, (long)&sa, (long)&addrlen);
    check_val("addr 127.0.0.1", (long)sa.addr, 0x0100007F);

    sc1(SYS_CLOSE, sfd);
}

/* ── bind03: EBADF for invalid fd ── */

static void test_bind03(void) {
    puts("\n[ltp/bind03]\n");

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;

    long r = sc3(SYS_BIND, -1, (long)&sa, 16);
    check_val("bind(-1) EBADF", r, -EBADF);

    r = sc3(SYS_BIND, 999, (long)&sa, 16);
    check_val("bind(999) EBADF", r, -EBADF);
}

/* ── bind04: ENOTSOCK for non-socket fd ── */

static void test_bind04(void) {
    puts("\n[ltp/bind04]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/bind04", O_RDWR | O_CREAT, 0644);
    check("open file", fd >= 0);
    if (fd < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;

    long r = sc3(SYS_BIND, fd, (long)&sa, 16);
    check_val("bind file ENOTSOCK", r, -ENOTSOCK);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/bind04");

    /* Pipe */
    long pipefd[2];
    r = sc1(SYS_PIPE, (long)pipefd);
    check_val("pipe", r, 0);
    if (r == 0) {
        r = sc3(SYS_BIND, pipefd[0], (long)&sa, 16);
        check_val("bind pipe ENOTSOCK", r, -ENOTSOCK);
        sc1(SYS_CLOSE, pipefd[0]);
        sc1(SYS_CLOSE, pipefd[1]);
    }
}

/* ── bind05: EADDRINUSE — double bind same port ── */

static void test_bind05(void) {
    puts("\n[ltp/bind05]\n");

    long sfd1 = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("socket 1", sfd1 >= 0);
    if (sfd1 < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    sa.addr = 0x0100007F;

    long r = sc3(SYS_BIND, sfd1, (long)&sa, 16);
    check_val("bind first", r, 0);

    /* Get assigned port */
    int addrlen = 16;
    sc3(SYS_GETSOCKNAME, sfd1, (long)&sa, (long)&addrlen);
    uint16_t port = sa.port;
    check("port assigned", port != 0);

    /* Second socket, same port */
    long sfd2 = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("socket 2", sfd2 >= 0);
    if (sfd2 < 0) { sc1(SYS_CLOSE, sfd1); return; }

    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    sa.port = port;
    sa.addr = 0x0100007F;

    r = sc3(SYS_BIND, sfd2, (long)&sa, 16);
    check_val("bind same port EADDRINUSE", r, -EADDRINUSE);

    sc1(SYS_CLOSE, sfd1);
    sc1(SYS_CLOSE, sfd2);
}

/* ── bind: EINVAL — bind already bound socket ── */

static void test_bind_einval_already_bound(void) {
    puts("\n[ltp/bind-einval-already-bound]\n");

    long sfd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("socket", sfd >= 0);
    if (sfd < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;

    long r = sc3(SYS_BIND, sfd, (long)&sa, 16);
    check_val("first bind", r, 0);

    /* Bind again — should fail EINVAL */
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    r = sc3(SYS_BIND, sfd, (long)&sa, 16);
    check_val("double bind EINVAL", r, -EINVAL);

    sc1(SYS_CLOSE, sfd);
}

/* ── bind: AF_UNIX basic ── */

static void test_bind_unix(void) {
    puts("\n[ltp/bind-unix]\n");

    long sfd = sc3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
    check("unix socket", sfd >= 0);
    if (sfd < 0) return;

    /* struct sockaddr_un: family(2) + path(108) */
    struct { uint16_t family; char path[108]; } sun;
    for (int i = 0; i < 110; i++) ((char *)&sun)[i] = 0;
    sun.family = AF_UNIX;
    /* /tmp/bind_unix_test */
    const char *p = "/tmp/bind_unix_test";
    int j = 0;
    while (p[j]) { sun.path[j] = p[j]; j++; }

    sc1(SYS_UNLINK, (long)sun.path);

    long r = sc3(SYS_BIND, sfd, (long)&sun, 2 + j + 1);
    check_val("bind AF_UNIX", r, 0);

    sc1(SYS_CLOSE, sfd);
    sc1(SYS_UNLINK, (long)sun.path);
}

TEST("ltp/bind01",                    test_bind01);
TEST("ltp/bind02",                    test_bind02);
TEST("ltp/bind03",                    test_bind03);
TEST("ltp/bind04",                    test_bind04);
TEST("ltp/bind05",                    test_bind05);
TEST("ltp/bind-einval-already-bound", test_bind_einval_already_bound);
TEST("ltp/bind-unix",                 test_bind_unix);
