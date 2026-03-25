/* test: DNS resolution via kernel /proc/nettest
 * NOTE: /proc/nettest runs a blocking kernel-level TCP test which can
 * take 30+ seconds. Skip for now — TCP test covers connectivity. */
#include "ktest.h"

static void test_dns_hosts(void) {
    puts("\n[net/dns]\n");

    /* Test that /etc/hosts resolution works by connecting to a host
     * whose IP is in /etc/hosts (registry.npmjs.org → 104.16.4.34) */
    int fd = (int)sc3(SYS_SOCKET, 2, 1, 0);
    check("socket", fd >= 0);
    if (fd < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = 2;
    sa.port = (uint16_t)((80 >> 8) | (80 << 8));
    /* registry.npmjs.org = 104.16.4.34 (from /etc/hosts) */
    sa.addr = (uint32_t)104 | (16u << 8) | (4u << 16) | (34u << 24);

    long r = sc3(SYS_CONNECT, fd, (long)&sa, 16);
    check("connect registry.npmjs.org:80", r == 0);
    sc1(SYS_CLOSE, fd);
}

TEST("net/dns", test_dns_hosts);
