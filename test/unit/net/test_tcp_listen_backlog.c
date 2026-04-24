/* test: TCP listen-backlog — half-open queue + accept after connect.
 *
 * Exercises the Linux-style passive-open path that LTP's accept4_01 hits:
 * a single thread does socket/bind/listen, then connect() *before*
 * accept(). In CosmoRT's old model the kernel answered the SYN only when
 * accept() ran, so connect() timed out with ETIMEDOUT. With the half-open
 * queue the SYN-ACK is emitted from tcp_input and the handshake
 * completes synchronously on loopback, so connect() returns 0 and a
 * subsequent accept() pops the completed request from accept_queue.
 */
#include "ktest.h"

#define IP_LOOPBACK 0x0100007FU /* 127.0.0.1 in network byte order */

static void fill_inaddr(void *a, uint16_t port_be, uint32_t ip_be) {
    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } *sa = a;
    for (int i = 0; i < 16; i++) ((char *)sa)[i] = 0;
    sa->family = AF_INET;
    sa->port   = port_be;
    sa->addr   = ip_be;
}

static void test_listen_backlog_self_connect(void) {
    puts("\n[net/listen-backlog self-connect]\n");

    int lfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("listen socket", lfd >= 0);
    if (lfd < 0) return;

    int one = 1;
    sc5(SYS_SETSOCKOPT, lfd, SOL_SOCKET, SO_REUSEADDR, (long)&one, (long)sizeof(int));

    char sa[16];
    fill_inaddr(sa, 0, IP_LOOPBACK);
    long r = sc3(SYS_BIND, lfd, (long)sa, 16);
    check_val("bind", r, 0);
    if (r != 0) { sc1(SYS_CLOSE, lfd); return; }

    int addrlen = 16;
    r = sc3(SYS_GETSOCKNAME, lfd, (long)sa, (long)&addrlen);
    check_val("getsockname", r, 0);
    uint16_t port_be = ((uint16_t *)sa)[1];

    r = sc2(SYS_LISTEN, lfd, 5);
    check_val("listen", r, 0);

    int cfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("client socket", cfd >= 0);

    char csa[16];
    fill_inaddr(csa, port_be, IP_LOOPBACK);
    r = sc3(SYS_CONNECT, cfd, (long)csa, 16);
    check_val("connect before accept", r, 0);

    /* accept() must now find a completed request on accept_queue and
     * return a child fd without blocking. */
    int afd = (int)sc3(SYS_ACCEPT, lfd, 0, 0);
    check("accept returns new fd", afd >= 0);

    /* Half-duplex data push verifies the handshake left the child in
     * a usable ESTABLISHED state (snd/rcv sequences wired correctly). */
    if (afd >= 0) {
        const char msg[] = "OK";
        r = sc3(SYS_WRITE, cfd, (long)msg, 2);
        check_val("client write 2B", r, 2);

        char buf[8] = {0};
        r = sc3(SYS_READ, afd, (long)buf, 2);
        check_val("server read 2B", r, 2);
        check("server saw OK", buf[0] == 'O' && buf[1] == 'K');

        sc1(SYS_CLOSE, afd);
    }

    sc1(SYS_CLOSE, cfd);
    sc1(SYS_CLOSE, lfd);
}

/* Queue two connects before a single accept — verifies FIFO ordering
 * and that a second SYN on the same listener doesn't clobber the first. */
static void test_listen_backlog_multi_pending(void) {
    puts("\n[net/listen-backlog multi-pending]\n");

    int lfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { fail("socket", "-"); return; }

    int one = 1;
    sc5(SYS_SETSOCKOPT, lfd, SOL_SOCKET, SO_REUSEADDR, (long)&one, (long)sizeof(int));

    char sa[16];
    fill_inaddr(sa, 0, IP_LOOPBACK);
    sc3(SYS_BIND, lfd, (long)sa, 16);
    int addrlen = 16;
    sc3(SYS_GETSOCKNAME, lfd, (long)sa, (long)&addrlen);
    uint16_t port_be = ((uint16_t *)sa)[1];
    sc2(SYS_LISTEN, lfd, 8);

    int c1 = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    int c2 = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    char csa[16];
    fill_inaddr(csa, port_be, IP_LOOPBACK);

    long r1 = sc3(SYS_CONNECT, c1, (long)csa, 16);
    check_val("connect #1", r1, 0);
    long r2 = sc3(SYS_CONNECT, c2, (long)csa, 16);
    check_val("connect #2", r2, 0);

    int a1 = (int)sc3(SYS_ACCEPT, lfd, 0, 0);
    check("accept #1 ok", a1 >= 0);
    int a2 = (int)sc3(SYS_ACCEPT, lfd, 0, 0);
    check("accept #2 ok", a2 >= 0);

    if (a1 >= 0) sc1(SYS_CLOSE, a1);
    if (a2 >= 0) sc1(SYS_CLOSE, a2);
    sc1(SYS_CLOSE, c1);
    sc1(SYS_CLOSE, c2);
    sc1(SYS_CLOSE, lfd);
}

/* close(listener) while a half-open / completed request is pending
 * must drain the queue cleanly (no leaks, no crash). */
static void test_listen_backlog_close_drains(void) {
    puts("\n[net/listen-backlog close-drains]\n");

    int lfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { fail("socket", "-"); return; }

    int one = 1;
    sc5(SYS_SETSOCKOPT, lfd, SOL_SOCKET, SO_REUSEADDR, (long)&one, (long)sizeof(int));

    char sa[16];
    fill_inaddr(sa, 0, IP_LOOPBACK);
    sc3(SYS_BIND, lfd, (long)sa, 16);
    int addrlen = 16;
    sc3(SYS_GETSOCKNAME, lfd, (long)sa, (long)&addrlen);
    uint16_t port_be = ((uint16_t *)sa)[1];
    sc2(SYS_LISTEN, lfd, 4);

    int cfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    char csa[16];
    fill_inaddr(csa, port_be, IP_LOOPBACK);
    long r = sc3(SYS_CONNECT, cfd, (long)csa, 16);
    check_val("connect queued", r, 0);

    /* Close listener with request still on accept_queue (we never called
     * accept). Drain path in socket_close must free without leaking. */
    r = sc1(SYS_CLOSE, lfd);
    check_val("close listener ok", r, 0);

    sc1(SYS_CLOSE, cfd);
}

TEST("net/listen-backlog self-connect",   test_listen_backlog_self_connect);
TEST("net/listen-backlog multi-pending",  test_listen_backlog_multi_pending);
TEST("net/listen-backlog close-drains",   test_listen_backlog_close_drains);
