/* CosmoRT Phase 16 — IPv6 ktests.
 *
 * Cover the user-visible behaviour of struct in6_addr, sockaddr_in6,
 * AF_INET6 socket(), bind/listen/accept/connect over loopback ::1, and
 * UDP6 send/recv over ::1. NDP/DAD/SLAAC are exercised via the kernel
 * test entry-points (no real IPv6-capable wire NIC in QEMU). */
#include "ktest.h"
#include "linux/in6.h"

#define AF_INET6                10
#define SOCK_STREAM             1
#define SOCK_DGRAM              2
#define IPPROTO_IPV6            41
#define IPV6_V6ONLY             26

static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static void make_loopback(struct in6_addr *a) {
    for (int i = 0; i < 15; i++) a->s6_addr[i] = 0;
    a->s6_addr[15] = 1;
}

/* 1: AF_INET6 socket() now succeeds (used to be EPROTONOSUPPORT). */
static void test_inet6_socket_create(void) {
    puts("\n[ipv6: socket(AF_INET6)]\n");
    long fd = sc3(SYS_SOCKET, AF_INET6, SOCK_STREAM, 0);
    check_ge("AF_INET6 SOCK_STREAM", fd, 0);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    long fd2 = sc3(SYS_SOCKET, AF_INET6, SOCK_DGRAM, 0);
    check_ge("AF_INET6 SOCK_DGRAM", fd2, 0);
    if (fd2 >= 0) sc1(SYS_CLOSE, fd2);
}

/* 2: bind ::1 on TCP6 + getsockname returns same address back. */
static void test_inet6_bind_loopback(void) {
    puts("\n[ipv6: bind ::1]\n");
    long fd = sc3(SYS_SOCKET, AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) { check("create", 0); return; }

    struct sockaddr_in6 sa = {0};
    sa.sin6_family = AF_INET6;
    sa.sin6_port = bswap16(45200);
    make_loopback(&sa.sin6_addr);
    long r = sc3(SYS_BIND, fd, (long)&sa, sizeof(sa));
    check_val("bind ::1:45200", r, 0);

    struct sockaddr_in6 got = {0};
    int slen = sizeof(got);
    long gr = sc3(SYS_GETSOCKNAME, fd, (long)&got, (long)&slen);
    check_val("getsockname rc", gr, 0);
    check_val("getsockname family", got.sin6_family, AF_INET6);
    check_val("getsockname port", got.sin6_port, bswap16(45200));
    int loopback_ok = (got.sin6_addr.s6_addr[15] == 1);
    for (int i = 0; i < 15; i++) if (got.sin6_addr.s6_addr[i]) loopback_ok = 0;
    check("getsockname addr ::1", loopback_ok);

    sc1(SYS_CLOSE, fd);
}

/* 3: bind :: (any) → port 0 ephemeral. */
static void test_inet6_bind_any_ephemeral(void) {
    puts("\n[ipv6: bind :: ephemeral]\n");
    long fd = sc3(SYS_SOCKET, AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) { check("create", 0); return; }

    struct sockaddr_in6 sa = {0};
    sa.sin6_family = AF_INET6;
    sa.sin6_port = 0;  /* request ephemeral */
    /* sin6_addr already zero = :: (any) */
    long r = sc3(SYS_BIND, fd, (long)&sa, sizeof(sa));
    check_val("bind :: ephemeral", r, 0);

    struct sockaddr_in6 got = {0};
    int slen = sizeof(got);
    sc3(SYS_GETSOCKNAME, fd, (long)&got, (long)&slen);
    check("ephemeral port assigned", got.sin6_port != 0);

    sc1(SYS_CLOSE, fd);
}

/* 4: TCP6 listen+accept+connect over ::1 loopback. */
static void test_tcp6_loopback_handshake(void) {
    puts("\n[ipv6: TCP6 ::1 handshake]\n");
    int lfd = (int)sc3(SYS_SOCKET, AF_INET6, SOCK_STREAM, 0);
    if (lfd < 0) { check("listen socket", 0); return; }

    struct sockaddr_in6 la = {0};
    la.sin6_family = AF_INET6;
    la.sin6_port = bswap16(45210);
    make_loopback(&la.sin6_addr);
    long br = sc3(SYS_BIND, lfd, (long)&la, sizeof(la));
    check_val("listener bind", br, 0);
    long lr = sc2(SYS_LISTEN, lfd, 4);
    check_val("listen", lr, 0);

    int pipefd[2] = {-1,-1};
    sc1(SYS_PIPE, (long)pipefd);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        int cfd = (int)sc3(SYS_SOCKET, AF_INET6, SOCK_STREAM, 0);
        struct sockaddr_in6 ra = {0};
        ra.sin6_family = AF_INET6;
        ra.sin6_port = bswap16(45210);
        make_loopback(&ra.sin6_addr);
        long cr = sc3(SYS_CONNECT, cfd, (long)&ra, sizeof(ra));
        sc3(SYS_WRITE, pipefd[1], (long)&cr, sizeof(cr));
        if (cr == 0) {
            const char m[] = "hi6";
            sc3(SYS_WRITE, cfd, (long)m, 3);
        }
        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT_GROUP, 0);
    }
    sc1(SYS_CLOSE, pipefd[1]);
    long crc = -99;
    sc3(SYS_READ, pipefd[0], (long)&crc, sizeof(crc));
    sc1(SYS_CLOSE, pipefd[0]);

    int newfd = (int)sc4(SYS_ACCEPT4, lfd, 0, 0, 0);
    check_ge("accept6", newfd, 0);
    if (newfd >= 0) {
        char buf[8] = {0};
        long n = sc3(SYS_READ, newfd, (long)buf, sizeof(buf));
        check_val("recv 3 bytes", n, 3);
        check("payload hi6",
              n == 3 && buf[0]=='h' && buf[1]=='i' && buf[2]=='6');
        sc1(SYS_CLOSE, newfd);
    }
    sc4(SYS_WAIT4, pid, 0, 0, 0);
    check_val("child connect rc", crc, 0);
    sc1(SYS_CLOSE, lfd);
}

/* 5: UDP6 send/recv over ::1. */
static void test_udp6_loopback_echo(void) {
    puts("\n[ipv6: UDP6 ::1 echo]\n");
    int sfd = (int)sc3(SYS_SOCKET, AF_INET6, SOCK_DGRAM, 0);
    if (sfd < 0) { check("server socket", 0); return; }
    struct sockaddr_in6 sa = {0};
    sa.sin6_family = AF_INET6; sa.sin6_port = bswap16(45221);
    make_loopback(&sa.sin6_addr);
    long br = sc3(SYS_BIND, sfd, (long)&sa, sizeof(sa));
    check_val("server bind", br, 0);

    int cfd = (int)sc3(SYS_SOCKET, AF_INET6, SOCK_DGRAM, 0);
    struct sockaddr_in6 da = {0};
    da.sin6_family = AF_INET6; da.sin6_port = bswap16(45221);
    make_loopback(&da.sin6_addr);
    const char m[] = "udp6";
    long sr = sc6(SYS_SENDTO, cfd, (long)m, 4, 0, (long)&da, sizeof(da));
    check_val("client sendto", sr, 4);

    char rbuf[16] = {0};
    long rr = sc6(SYS_RECVFROM, sfd, (long)rbuf, sizeof(rbuf), 0, 0, 0);
    check_val("server recv", rr, 4);
    check("payload udp6", rr == 4 && rbuf[0]=='u' && rbuf[1]=='d' &&
                          rbuf[2]=='p' && rbuf[3]=='6');

    sc1(SYS_CLOSE, cfd);
    sc1(SYS_CLOSE, sfd);
}

/* 6: bind sockaddr_in6 with addrlen<28 → EINVAL. */
static void test_inet6_bind_short_addr(void) {
    puts("\n[ipv6: bind short addrlen]\n");
    long fd = sc3(SYS_SOCKET, AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) { check("create", 0); return; }
    struct sockaddr_in6 sa = {0};
    sa.sin6_family = AF_INET6;
    long r = sc3(SYS_BIND, fd, (long)&sa, 8);
    check_val("bind 8 bytes -> EINVAL", r, -22);
    sc1(SYS_CLOSE, fd);
}

/* 7: AF_INET6 with mismatched sin6_family in addr → EAFNOSUPPORT/EINVAL. */
static void test_inet6_bind_wrong_family(void) {
    puts("\n[ipv6: bind wrong family]\n");
    long fd = sc3(SYS_SOCKET, AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) { check("create", 0); return; }
    struct sockaddr_in6 sa = {0};
    sa.sin6_family = 2; /* AF_INET, mismatched */
    long r = sc3(SYS_BIND, fd, (long)&sa, sizeof(sa));
    check("bind wrong family rejected", r != 0);
    sc1(SYS_CLOSE, fd);
}

/* 8: IPV6_V6ONLY default (1) — getsockopt confirms. */
static void test_inet6_v6only_default(void) {
    puts("\n[ipv6: IPV6_V6ONLY default]\n");
    long fd = sc3(SYS_SOCKET, AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) { check("create", 0); return; }
    int val = -1; int slen = sizeof(int);
    long r = sc5(SYS_GETSOCKOPT, fd, IPPROTO_IPV6, IPV6_V6ONLY,
                 (long)&val, (long)&slen);
    check_val("getsockopt rc", r, 0);
    check_val("default V6ONLY=1", val, 1);

    /* Toggle off, read back. */
    int zero = 0;
    sc5(SYS_SETSOCKOPT, fd, IPPROTO_IPV6, IPV6_V6ONLY, (long)&zero, sizeof(int));
    val = -1;
    sc5(SYS_GETSOCKOPT, fd, IPPROTO_IPV6, IPV6_V6ONLY, (long)&val, (long)&slen);
    check_val("after clear V6ONLY=0", val, 0);
    sc1(SYS_CLOSE, fd);
}

/* 9: Two AF_INET6 sockets cannot bind same port (EADDRINUSE). */
static void test_inet6_bind_conflict(void) {
    puts("\n[ipv6: bind conflict]\n");
    long fa = sc3(SYS_SOCKET, AF_INET6, SOCK_STREAM, 0);
    long fb = sc3(SYS_SOCKET, AF_INET6, SOCK_STREAM, 0);
    if (fa < 0 || fb < 0) { check("create", 0);
        if (fa >= 0) sc1(SYS_CLOSE, fa);
        if (fb >= 0) sc1(SYS_CLOSE, fb);
        return;
    }
    struct sockaddr_in6 sa = {0};
    sa.sin6_family = AF_INET6;
    sa.sin6_port = bswap16(45230);
    make_loopback(&sa.sin6_addr);
    long ra = sc3(SYS_BIND, fa, (long)&sa, sizeof(sa));
    check_val("first bind", ra, 0);
    long rb = sc3(SYS_BIND, fb, (long)&sa, sizeof(sa));
    /* EADDRINUSE = 98 */
    check_val("second bind EADDRINUSE", rb, -98);
    sc1(SYS_CLOSE, fa);
    sc1(SYS_CLOSE, fb);
}

/* 10: NDP fallback — sending to a non-loopback address must complete
 * cleanly (kernel may return -EAGAIN or -EHOSTUNREACH on no NA arrival;
 * what matters is no crash and a well-typed errno). */
static void test_ipv6_send_routes_to_default(void) {
    puts("\n[ipv6: udp6 to non-loopback gracefully fails]\n");
    long fd = sc3(SYS_SOCKET, AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) { check("create", 0); return; }

    /* 2001:db8::1 — RFC 3849 documentation prefix, never reachable. */
    struct sockaddr_in6 da = {0};
    da.sin6_family = AF_INET6;
    da.sin6_port   = bswap16(45299);
    da.sin6_addr.s6_addr[0] = 0x20;
    da.sin6_addr.s6_addr[1] = 0x01;
    da.sin6_addr.s6_addr[2] = 0x0d;
    da.sin6_addr.s6_addr[3] = 0xb8;
    da.sin6_addr.s6_addr[15] = 1;

    const char m[] = "ndp";
    long sr = sc6(SYS_SENDTO, fd, (long)m, 3, 0, (long)&da, sizeof(da));
    /* sr is either 3 (sent) or a negative errno; either way no crash. */
    check("send returns cleanly (no crash)", sr == 3 || sr < 0);
    sc1(SYS_CLOSE, fd);
}

TEST("ipv6/socket-create",         test_inet6_socket_create);
TEST("ipv6/bind-loopback",         test_inet6_bind_loopback);
TEST("ipv6/bind-any-ephemeral",    test_inet6_bind_any_ephemeral);
TEST("ipv6/tcp6-loopback",         test_tcp6_loopback_handshake);
TEST("ipv6/udp6-loopback",         test_udp6_loopback_echo);
TEST("ipv6/bind-short-addr",       test_inet6_bind_short_addr);
TEST("ipv6/bind-wrong-family",     test_inet6_bind_wrong_family);
TEST("ipv6/v6only-default",        test_inet6_v6only_default);
TEST("ipv6/bind-conflict",         test_inet6_bind_conflict);
TEST("ipv6/non-loopback-send",     test_ipv6_send_routes_to_default);
