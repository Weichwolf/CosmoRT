/* test: Socket lifecycle — close cleanup, fork refcount, bind reuse */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

/* Create socket, close, create new → no EADDRINUSE leak */
static void test_socket_close_cleanup(void) {
    puts("\n[net/lifecycle: close cleanup]\n");

    /* Create and immediately close a bound UDP socket */
    int fd1 = (int)sc3(SYS_SOCKET, AF_INET, SOCK_DGRAM, 0);
    check("socket #1", fd1 >= 0);
    if (fd1 < 0) return;

    /* Bind to ephemeral port (port 0) */
    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    sa.port = 0;
    sa.addr = 0; /* INADDR_ANY */
    long r = sc3(SYS_BIND, fd1, (long)&sa, 16);
    check_val("bind #1 ephemeral", r, 0);

    sc1(SYS_CLOSE, fd1);

    /* Create second socket, should succeed (no leak) */
    int fd2 = (int)sc3(SYS_SOCKET, AF_INET, SOCK_DGRAM, 0);
    check("socket #2 after close", fd2 >= 0);
    if (fd2 >= 0) sc1(SYS_CLOSE, fd2);
}

/* socketpair, fork, parent close → child still works */
static void test_socket_fork_refcount(void) {
    puts("\n[net/lifecycle: fork refcount]\n");

    int sv[2] = { -1, -1 };
    long r = sc4(SYS_SOCKETPAIR, AF_UNIX, SOCK_STREAM, 0, (long)sv);
    check_val("socketpair", r, 0);
    if (r != 0) return;

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) { sc1(SYS_CLOSE, sv[0]); sc1(SYS_CLOSE, sv[1]); return; }

    if (pid == 0) {
        /* Child: close sv[0], read from sv[1] */
        sc1(SYS_CLOSE, sv[0]);
        char buf[8];
        long rd = sc3(SYS_READ, sv[1], (long)buf, 8);
        /* Parent writes "OK" then closes, child should get it */
        sc1(SYS_CLOSE, sv[1]);
        sc1(SYS_EXIT_GROUP, (rd == 2 && buf[0] == 'O') ? 0 : 1);
        __builtin_unreachable();
    }

    /* Parent: close sv[1], write to sv[0] */
    sc1(SYS_CLOSE, sv[1]);
    sc3(SYS_WRITE, sv[0], (long)"OK", 2);
    sc1(SYS_CLOSE, sv[0]);

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));
    check_val("child exit 0 (refcount ok)", WEXITSTATUS(wstatus), 0);
}

/* Bind UDP port 0, close, rebind → works (no port leak) */
static void test_udp_bind_reuse(void) {
    puts("\n[net/lifecycle: udp bind reuse]\n");

    for (int i = 0; i < 3; i++) {
        int fd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_DGRAM, 0);
        check("socket", fd >= 0);
        if (fd < 0) return;

        struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
        for (int j = 0; j < 16; j++) ((char *)&sa)[j] = 0;
        sa.family = AF_INET;
        sa.port = 0; /* ephemeral */
        sa.addr = 0;

        long r = sc3(SYS_BIND, fd, (long)&sa, 16);
        if (i == 0) check_val("bind #1", r, 0);
        else if (i == 1) check_val("bind #2 after close", r, 0);
        else check_val("bind #3 after close", r, 0);

        sc1(SYS_CLOSE, fd);
    }
}

TEST("net/socket-close-cleanup", test_socket_close_cleanup);
TEST("net/socket-fork-refcount", test_socket_fork_refcount);
TEST("net/udp-bind-reuse", test_udp_bind_reuse);
