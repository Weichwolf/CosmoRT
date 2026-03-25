/* test: UDP — DNS query/response via QEMU slirp DNS (10.0.2.3) */
#include "ktest.h"

static void test_udp_dns(void) {
    puts("\n[net/udp]\n");

    int fd = (int)sc3(SYS_SOCKET, 2, 2 /* SOCK_DGRAM */, 0);
    check("udp socket", fd >= 0);
    if (fd < 0) return;

    /* DNS query: example.com A record */
    uint8_t query[] = {
        0xAB, 0xCD, 0x01, 0x00,   /* ID, Flags (RD) */
        0x00, 0x01, 0x00, 0x00,   /* QDCOUNT=1, ANCOUNT=0 */
        0x00, 0x00, 0x00, 0x00,   /* NSCOUNT=0, ARCOUNT=0 */
        7, 'e','x','a','m','p','l','e',
        3, 'c','o','m', 0,
        0x00, 0x01,               /* QTYPE: A */
        0x00, 0x01                /* QCLASS: IN */
    };

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } dst;
    for (int i = 0; i < 16; i++) ((char *)&dst)[i] = 0;
    dst.family = 2;
    dst.port = (uint16_t)((53 >> 8) | (53 << 8));
    dst.addr = (uint32_t)10 | (0u << 8) | (2u << 16) | (3u << 24);

    long r = sc6(SYS_SENDTO, fd, (long)query, (long)sizeof(query), 0, (long)&dst, 16);
    check("sendto DNS", r == (long)sizeof(query));

    /* Recv response */
    char rbuf[512];
    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } from;
    int fromlen = 16;
    r = sc6(SYS_RECVFROM, fd, (long)rbuf, sizeof(rbuf), 0, (long)&from, (long)&fromlen);
    check("recvfrom > 12", r > 12);
    if (r > 12) {
        check("response ID match", rbuf[0]==(char)0xAB && rbuf[1]==(char)0xCD);
        check("QR=1 (response)", (rbuf[2] & 0x80) != 0);
        check("RCODE=0 (no error)", (rbuf[3] & 0x0F) == 0);
    }

    sc1(SYS_CLOSE, fd);
}

TEST("net/udp", test_udp_dns);
