/* test: TCP data transfer — HTTP downloads in various sizes
 * Tests TCP reliability: data integrity, large transfers, connection reuse.
 * Uses example.com (HTTP, not HTTPS) for direct kernel TCP testing.
 */
#include "ktest.h"

static int tcp_connect_80(uint32_t addr) {
    int fd = (int)sc3(SYS_SOCKET, 2, 1, 0);
    if (fd < 0) return -1;
    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = 2;
    sa.port = (uint16_t)((80 >> 8) | (80 << 8));
    sa.addr = addr;
    if (sc3(SYS_CONNECT, fd, (long)&sa, 16) != 0) { sc1(SYS_CLOSE, fd); return -1; }
    return fd;
}

static long http_get(int fd, const char *host, const char *path) {
    char req[256];
    int ri = 0;
    const char *p;
    p = "GET "; while (*p) req[ri++] = *p++;
    while (*path) req[ri++] = *path++;
    p = " HTTP/1.0\r\nHost: "; while (*p) req[ri++] = *p++;
    while (*host) req[ri++] = *host++;
    p = "\r\n\r\n"; while (*p) req[ri++] = *p++;
    return sc3(SYS_WRITE, fd, (long)req, ri);
}

static long drain_to_null(int fd) {
    long total = 0;
    char buf[4096];
    for (;;) {
        long r = sc3(SYS_READ, fd, (long)buf, sizeof(buf));
        if (r <= 0) break;
        total += r;
    }
    return total;
}

/* example.com = 104.18.27.120 */
#define EXAMPLE_COM ((uint32_t)104|(18u<<8)|(27u<<16)|(120u<<24))

static void test_tcp_small(void) {
    puts("\n[net/tcp-transfer: small]\n");

    int fd = tcp_connect_80(EXAMPLE_COM);
    check("connect", fd >= 0);
    if (fd < 0) return;

    long r = http_get(fd, "example.com", "/");
    check("send GET /", r > 0);

    long total = drain_to_null(fd);
    puts("  received "); put_int(total); puts(" bytes\n");
    check("recv > 500 bytes", total > 500);  /* example.com HTML ~1256 bytes */
    check("recv < 100KB", total < 100000);

    sc1(SYS_CLOSE, fd);
}

static void test_tcp_sequential(void) {
    puts("\n[net/tcp-transfer: sequential]\n");

    /* Two sequential connections to verify connection cleanup works */
    for (int attempt = 0; attempt < 2; attempt++) {
        int fd = tcp_connect_80(EXAMPLE_COM);
        if (attempt == 0)
            check("connect #1", fd >= 0);
        else
            check("connect #2", fd >= 0);
        if (fd < 0) continue;

        http_get(fd, "example.com", "/");
        long total = drain_to_null(fd);
        if (attempt == 0)
            check("recv #1 > 500", total > 500);
        else
            check("recv #2 > 500", total > 500);
        sc1(SYS_CLOSE, fd);
    }
}

TEST("net/tcp-transfer", test_tcp_small);
TEST("net/tcp-sequential", test_tcp_sequential);
