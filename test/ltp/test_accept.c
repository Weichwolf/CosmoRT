/* LTP accept01/accept02/accept03/accept4_01 — ported to ktest */
#include "ktest.h"

#define SOCK_CLOEXEC    0x80000
#define SOCK_NONBLOCK   0x800
#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

/* ── accept01: error cases ─────────────────────── */

/* EBADF for invalid fd */
static void test_accept01_ebadf(void) {
    puts("\n[ltp/accept01-ebadf]\n");
    long r = sc3(SYS_ACCEPT, 400, 0, 0);
    check_val("accept bad fd EBADF", r, -EBADF);
}

/* EINVAL for invalid sockaddr pointer */
static void test_accept01_einval_buf(void) {
    puts("\n[ltp/accept01-einval-buf]\n");

    int sfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("socket", sfd >= 0);
    if (sfd < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    long r = sc3(SYS_BIND, sfd, (long)&sa, 16);
    check_val("bind", r, 0);

    /* accept with invalid buffer (addr=3) — no listen, should EINVAL */
    int salen = 16;
    r = sc3(SYS_ACCEPT, sfd, 3, (long)&salen);
    check_val("accept invalid buf EINVAL", r, -EINVAL);

    sc1(SYS_CLOSE, sfd);
}

/* EINVAL for invalid salen (too small) */
static void test_accept01_einval_salen(void) {
    puts("\n[ltp/accept01-einval-salen]\n");

    int sfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("socket", sfd >= 0);
    if (sfd < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    sc3(SYS_BIND, sfd, (long)&sa, 16);

    int salen = 1;
    long r = sc3(SYS_ACCEPT, sfd, (long)&sa, (long)&salen);
    check_val("accept salen=1 EINVAL", r, -EINVAL);

    sc1(SYS_CLOSE, sfd);
}

/* EINVAL for no listen (accept on bound but not listening socket) */
static void test_accept01_einval_nolisten(void) {
    puts("\n[ltp/accept01-einval-nolisten]\n");

    int sfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("socket", sfd >= 0);
    if (sfd < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    sc3(SYS_BIND, sfd, (long)&sa, 16);

    int salen = 16;
    long r = sc3(SYS_ACCEPT, sfd, (long)&sa, (long)&salen);
    check_val("accept no-listen EINVAL", r, -EINVAL);

    sc1(SYS_CLOSE, sfd);
}

/* EOPNOTSUPP for UDP socket */
static void test_accept01_eopnotsupp(void) {
    puts("\n[ltp/accept01-eopnotsupp]\n");

    int ufd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_DGRAM, 0);
    check("UDP socket", ufd >= 0);
    if (ufd < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    sc3(SYS_BIND, ufd, (long)&sa, 16);

    int salen = 16;
    long r = sc3(SYS_ACCEPT, ufd, (long)&sa, (long)&salen);
    check_val("accept UDP EOPNOTSUPP", r, -EOPNOTSUPP);

    sc1(SYS_CLOSE, ufd);
}

/* ── accept03: ENOTSOCK for non-socket fds ─────── */

/* accept on regular file */
static void test_accept03_file(void) {
    puts("\n[ltp/accept03-file]\n");
    long fd = sc3(SYS_OPEN, (long)"/dev/null", O_RDONLY, 0);
    check("open /dev/null", fd >= 0);
    if (fd < 0) return;
    long r = sc3(SYS_ACCEPT, fd, 0, 0);
    check_val("accept file ENOTSOCK", r, -ENOTSOCK);
    sc1(SYS_CLOSE, fd);
}

/* accept on directory */
static void test_accept03_dir(void) {
    puts("\n[ltp/accept03-dir]\n");
    long fd = sc3(SYS_OPEN, (long)"/tmp", O_RDONLY | O_DIRECTORY, 0);
    check("open /tmp", fd >= 0);
    if (fd < 0) return;
    long r = sc3(SYS_ACCEPT, fd, 0, 0);
    check_val("accept dir ENOTSOCK", r, -ENOTSOCK);
    sc1(SYS_CLOSE, fd);
}

/* accept on pipe */
static void test_accept03_pipe(void) {
    puts("\n[ltp/accept03-pipe]\n");
    long pipefd[2];
    long r = sc1(SYS_PIPE, (long)pipefd);
    check_val("pipe", r, 0);
    if (r != 0) return;
    r = sc3(SYS_ACCEPT, pipefd[0], 0, 0);
    check_val("accept pipe ENOTSOCK", r, -ENOTSOCK);
    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

/* accept on epoll fd */
static void test_accept03_epoll(void) {
    puts("\n[ltp/accept03-epoll]\n");
    long efd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", efd >= 0);
    if (efd < 0) return;
    long r = sc3(SYS_ACCEPT, efd, 0, 0);
    check_val("accept epoll ENOTSOCK", r, -ENOTSOCK);
    sc1(SYS_CLOSE, efd);
}

/* accept on eventfd */
static void test_accept03_eventfd(void) {
    puts("\n[ltp/accept03-eventfd]\n");
    long efd = sc2(SYS_EVENTFD2, 0, 0);
    check("eventfd2", efd >= 0);
    if (efd < 0) return;
    long r = sc3(SYS_ACCEPT, efd, 0, 0);
    check_val("accept eventfd ENOTSOCK", r, -ENOTSOCK);
    sc1(SYS_CLOSE, efd);
}

/* accept on timerfd */
static void test_accept03_timerfd(void) {
    puts("\n[ltp/accept03-timerfd]\n");
    long tfd = sc2(SYS_TIMERFD_CREATE, 1 /* CLOCK_MONOTONIC */, 0);
    check("timerfd_create", tfd >= 0);
    if (tfd < 0) return;
    long r = sc3(SYS_ACCEPT, tfd, 0, 0);
    check_val("accept timerfd ENOTSOCK", r, -ENOTSOCK);
    sc1(SYS_CLOSE, tfd);
}

/* accept on inotify fd */
static void test_accept03_inotify(void) {
    puts("\n[ltp/accept03-inotify]\n");
    long ifd = sc1(SYS_INOTIFY_INIT1, 0);
    check("inotify_init1", ifd >= 0);
    if (ifd < 0) return;
    long r = sc3(SYS_ACCEPT, ifd, 0, 0);
    check_val("accept inotify ENOTSOCK", r, -ENOTSOCK);
    sc1(SYS_CLOSE, ifd);
}

/* accept on memfd */
static void test_accept03_memfd(void) {
    puts("\n[ltp/accept03-memfd]\n");
    long mfd = sc2(SYS_MEMFD_CREATE, (long)"test", 0);
    check("memfd_create", mfd >= 0);
    if (mfd < 0) return;
    long r = sc3(SYS_ACCEPT, mfd, 0, 0);
    check_val("accept memfd ENOTSOCK", r, -ENOTSOCK);
    sc1(SYS_CLOSE, mfd);
}

/* ── accept02: basic loopback accept (will timeout — known bug) ── */

static void test_accept02_loopback(void) {
    puts("\n[ltp/accept02-loopback]\n");

    int lfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("listen socket", lfd >= 0);
    if (lfd < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    sa.addr = 0x0100007F; /* 127.0.0.1 */

    long r = sc3(SYS_BIND, lfd, (long)&sa, 16);
    check_val("bind loopback", r, 0);
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

    /* Parent: accept */
    int afd = (int)sc3(SYS_ACCEPT, lfd, 0, 0);
    if (afd >= 0) {
        pass("loopback accept");
        sc1(SYS_CLOSE, afd);
    } else {
        fail("loopback accept", "accept failed (loopback bug)");
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));

    sc1(SYS_CLOSE, lfd);
}

/* ── accept4_01: SOCK_CLOEXEC and SOCK_NONBLOCK flags ── */

static void test_accept4_cloexec(void) {
    puts("\n[ltp/accept4-cloexec]\n");

    int lfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
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
        int cfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
        struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } csa;
        for (int i = 0; i < 16; i++) ((char *)&csa)[i] = 0;
        csa.family = AF_INET;
        csa.port = port;
        csa.addr = 0x0100007F;
        sc3(SYS_CONNECT, cfd, (long)&csa, 16);
        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    /* accept4 with SOCK_CLOEXEC */
    int afd = (int)sc4(SYS_ACCEPT4, lfd, 0, 0, SOCK_CLOEXEC);
    if (afd >= 0) {
        long flags = sc2(SYS_FCNTL, afd, F_GETFD);
        check("accept4 FD_CLOEXEC set", (flags & FD_CLOEXEC) != 0);
        sc1(SYS_CLOSE, afd);
    } else {
        fail("accept4 SOCK_CLOEXEC", "accept4 failed (loopback bug)");
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    sc1(SYS_CLOSE, lfd);
}

static void test_accept4_nonblock(void) {
    puts("\n[ltp/accept4-nonblock]\n");

    int lfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
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
        int cfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
        struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } csa;
        for (int i = 0; i < 16; i++) ((char *)&csa)[i] = 0;
        csa.family = AF_INET;
        csa.port = port;
        csa.addr = 0x0100007F;
        sc3(SYS_CONNECT, cfd, (long)&csa, 16);
        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    /* accept4 with SOCK_NONBLOCK */
    int afd = (int)sc4(SYS_ACCEPT4, lfd, 0, 0, SOCK_NONBLOCK);
    if (afd >= 0) {
        long fl = sc2(SYS_FCNTL, afd, F_GETFL);
        check("accept4 O_NONBLOCK set", (fl & O_NONBLOCK) != 0);
        sc1(SYS_CLOSE, afd);
    } else {
        fail("accept4 SOCK_NONBLOCK", "accept4 failed (loopback bug)");
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    sc1(SYS_CLOSE, lfd);
}

static void test_accept4_both(void) {
    puts("\n[ltp/accept4-both]\n");

    int lfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
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
        int cfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
        struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } csa;
        for (int i = 0; i < 16; i++) ((char *)&csa)[i] = 0;
        csa.family = AF_INET;
        csa.port = port;
        csa.addr = 0x0100007F;
        sc3(SYS_CONNECT, cfd, (long)&csa, 16);
        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    /* accept4 with SOCK_CLOEXEC | SOCK_NONBLOCK */
    int afd = (int)sc4(SYS_ACCEPT4, lfd, 0, 0, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (afd >= 0) {
        long fd_flags = sc2(SYS_FCNTL, afd, F_GETFD);
        long fl_flags = sc2(SYS_FCNTL, afd, F_GETFL);
        check("accept4 FD_CLOEXEC set", (fd_flags & FD_CLOEXEC) != 0);
        check("accept4 O_NONBLOCK set", (fl_flags & O_NONBLOCK) != 0);
        sc1(SYS_CLOSE, afd);
    } else {
        fail("accept4 CLOEXEC|NONBLOCK", "accept4 failed (loopback bug)");
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    sc1(SYS_CLOSE, lfd);
}

static void test_accept4_noflags(void) {
    puts("\n[ltp/accept4-noflags]\n");

    int lfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
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
        int cfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
        struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } csa;
        for (int i = 0; i < 16; i++) ((char *)&csa)[i] = 0;
        csa.family = AF_INET;
        csa.port = port;
        csa.addr = 0x0100007F;
        sc3(SYS_CONNECT, cfd, (long)&csa, 16);
        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    /* accept4 with flags=0 — neither CLOEXEC nor NONBLOCK */
    int afd = (int)sc4(SYS_ACCEPT4, lfd, 0, 0, 0);
    if (afd >= 0) {
        long fd_flags = sc2(SYS_FCNTL, afd, F_GETFD);
        long fl_flags = sc2(SYS_FCNTL, afd, F_GETFL);
        check("accept4 no FD_CLOEXEC", (fd_flags & FD_CLOEXEC) == 0);
        check("accept4 no O_NONBLOCK", (fl_flags & O_NONBLOCK) == 0);
        sc1(SYS_CLOSE, afd);
    } else {
        fail("accept4 flags=0", "accept4 failed (loopback bug)");
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    sc1(SYS_CLOSE, lfd);
}

TEST("ltp/accept01-ebadf",       test_accept01_ebadf);
TEST("ltp/accept01-einval-buf",  test_accept01_einval_buf);
TEST("ltp/accept01-einval-salen", test_accept01_einval_salen);
TEST("ltp/accept01-einval-nolisten", test_accept01_einval_nolisten);
TEST("ltp/accept01-eopnotsupp",  test_accept01_eopnotsupp);
TEST("ltp/accept02-loopback",    test_accept02_loopback);
TEST("ltp/accept03-file",        test_accept03_file);
TEST("ltp/accept03-dir",         test_accept03_dir);
TEST("ltp/accept03-pipe",        test_accept03_pipe);
TEST("ltp/accept03-epoll",       test_accept03_epoll);
TEST("ltp/accept03-eventfd",     test_accept03_eventfd);
TEST("ltp/accept03-timerfd",     test_accept03_timerfd);
TEST("ltp/accept03-inotify",     test_accept03_inotify);
TEST("ltp/accept03-memfd",       test_accept03_memfd);
TEST("ltp/accept4-cloexec",      test_accept4_cloexec);
TEST("ltp/accept4-nonblock",     test_accept4_nonblock);
TEST("ltp/accept4-both",         test_accept4_both);
TEST("ltp/accept4-noflags",      test_accept4_noflags);
