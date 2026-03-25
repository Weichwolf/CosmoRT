/* test: UDP Per-Socket Demux — two sockets, verify packets route correctly.
 * Uses QEMU slirp DNS (10.0.2.3:53) as echo-like target:
 * each socket sends a DNS query with unique ID, verifies correct routing.
 */
#include "ktest.h"

/* DNS query template for A record lookup of "example.com" */
static void build_dns_query(uint8_t *buf, int *len, uint16_t txid) {
    uint8_t q[] = {
        (uint8_t)(txid >> 8), (uint8_t)txid,
        0x01, 0x00,             /* Flags: RD */
        0x00, 0x01, 0x00, 0x00, /* QDCOUNT=1 */
        0x00, 0x00, 0x00, 0x00, /* NSCOUNT=0, ARCOUNT=0 */
        7, 'e','x','a','m','p','l','e',
        3, 'c','o','m', 0,
        0x00, 0x01,             /* QTYPE: A */
        0x00, 0x01              /* QCLASS: IN */
    };
    for (int i = 0; i < (int)sizeof(q); i++) buf[i] = q[i];
    *len = (int)sizeof(q);
}

static void test_net_udp_echo(void) {
    puts("\n[net/udp_echo]\n");

    /* sockaddr for QEMU slirp DNS 10.0.2.3:53 */
    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } dns_addr;
    for (int i = 0; i < 16; i++) ((char *)&dns_addr)[i] = 0;
    dns_addr.family = 2;
    dns_addr.port = (uint16_t)((53 >> 8) | (53 << 8));
    dns_addr.addr = (uint32_t)10 | (0u << 8) | (2u << 16) | (3u << 24);

    /* ── Socket A: bind to port 10053 ── */
    int fd_a = (int)sc3(SYS_SOCKET, 2, 2, 0);
    check("socket A", fd_a >= 0);
    if (fd_a < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } bind_a;
    for (int i = 0; i < 16; i++) ((char *)&bind_a)[i] = 0;
    bind_a.family = 2;
    bind_a.port = (uint16_t)((10053 >> 8) | ((10053 & 0xFF) << 8));
    long r = sc3(SYS_BIND, fd_a, (long)&bind_a, 16);
    check("bind A:10053", r == 0);

    /* ── Socket B: bind to port 10054 ── */
    int fd_b = (int)sc3(SYS_SOCKET, 2, 2, 0);
    check("socket B", fd_b >= 0);
    if (fd_b < 0) { sc1(SYS_CLOSE, fd_a); return; }

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } bind_b;
    for (int i = 0; i < 16; i++) ((char *)&bind_b)[i] = 0;
    bind_b.family = 2;
    bind_b.port = (uint16_t)((10054 >> 8) | ((10054 & 0xFF) << 8));
    r = sc3(SYS_BIND, fd_b, (long)&bind_b, 16);
    check("bind B:10054", r == 0);

    /* ── Send DNS queries with unique IDs ── */
    uint8_t query_a[64], query_b[64];
    int qlen_a, qlen_b;
    build_dns_query(query_a, &qlen_a, 0x1111);
    build_dns_query(query_b, &qlen_b, 0x2222);

    r = sc6(SYS_SENDTO, fd_a, (long)query_a, qlen_a, 0, (long)&dns_addr, 16);
    check("sendto A", r == qlen_a);

    r = sc6(SYS_SENDTO, fd_b, (long)query_b, qlen_b, 0, (long)&dns_addr, 16);
    check("sendto B", r == qlen_b);

    /* ── Receive on socket A — should get ID 0x1111 ── */
    char rbuf_a[512];
    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } from_a;
    int fromlen_a = 16;
    r = sc6(SYS_RECVFROM, fd_a, (long)rbuf_a, sizeof(rbuf_a), 0, (long)&from_a, (long)&fromlen_a);
    check("recvfrom A > 12", r > 12);
    if (r > 12) {
        check("A: ID=0x1111", rbuf_a[0]==(char)0x11 && rbuf_a[1]==(char)0x11);
        check("A: QR=1", (rbuf_a[2] & 0x80) != 0);
    }

    /* ── Receive on socket B — should get ID 0x2222 ── */
    char rbuf_b[512];
    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } from_b;
    int fromlen_b = 16;
    r = sc6(SYS_RECVFROM, fd_b, (long)rbuf_b, sizeof(rbuf_b), 0, (long)&from_b, (long)&fromlen_b);
    check("recvfrom B > 12", r > 12);
    if (r > 12) {
        check("B: ID=0x2222", rbuf_b[0]==(char)0x22 && rbuf_b[1]==(char)0x22);
        check("B: QR=1", (rbuf_b[2] & 0x80) != 0);
    }

    /* ── Cleanup ── */
    sc1(SYS_CLOSE, fd_a);
    sc1(SYS_CLOSE, fd_b);
}

TEST("net/udp_echo", test_net_udp_echo);
