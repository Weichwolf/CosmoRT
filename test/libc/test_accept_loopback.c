/* test: accept() on loopback — reproduces musl socket test failure */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

/* htons for port byte-swap (little-endian x86_64) */
static inline uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

/*
 * accept() on 127.0.0.1 returns EAGAIN because kernel checks net_gw_ip==0
 * and rejects loopback connections. This test proves the bug.
 */
static void test_accept_loopback(void) {
    puts("\n[net/accept-loopback]\n");

    int lfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("listen socket", lfd >= 0);
    if (lfd < 0) return;

    /* Bind to 127.0.0.1:0 */
    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    sa.port = 0;
    sa.addr = 0x0100007F; /* 127.0.0.1 in network byte order */

    long r = sc3(SYS_BIND, lfd, (long)&sa, 16);
    check_val("bind loopback", r, 0);
    if (r != 0) { sc1(SYS_CLOSE, lfd); return; }

    /* Get assigned port */
    int addrlen = 16;
    r = sc3(SYS_GETSOCKNAME, lfd, (long)&sa, (long)&addrlen);
    check_val("getsockname", r, 0);
    uint16_t port = sa.port; /* already network byte order */

    r = sc2(SYS_LISTEN, lfd, 5);
    check_val("listen", r, 0);

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) { sc1(SYS_CLOSE, lfd); return; }

    if (pid == 0) {
        /* Child: connect to 127.0.0.1:port, write, exit */
        sc1(SYS_CLOSE, lfd);
        int cfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
        struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } csa;
        for (int i = 0; i < 16; i++) ((char *)&csa)[i] = 0;
        csa.family = AF_INET;
        csa.port = port;
        csa.addr = 0x0100007F;
        long cr = sc3(SYS_CONNECT, cfd, (long)&csa, 16);
        if (cr == 0)
            sc3(SYS_WRITE, cfd, (long)"hi", 2);
        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT_GROUP, (cr == 0) ? 0 : 1);
        __builtin_unreachable();
    }

    /* Parent: accept — this is the bug: returns EAGAIN on loopback */
    int afd = (int)sc3(SYS_ACCEPT, lfd, 0, 0);
    check("accept loopback", afd >= 0);

    if (afd >= 0) {
        char buf[4] = {0};
        sc3(SYS_READ, afd, (long)buf, 2);
        check("read from accepted", buf[0] == 'h' && buf[1] == 'i');
        sc1(SYS_CLOSE, afd);
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));
    check_val("child connect ok", WEXITSTATUS(wstatus), 0);

    sc1(SYS_CLOSE, lfd);
}

TEST("net/accept-loopback", test_accept_loopback);
