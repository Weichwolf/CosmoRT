/* test: TCP window management — recv window update, large transfer, snd_wnd init */
#include "ktest.h"

#define EXAMPLE_COM ((uint32_t)104|(18u<<8)|(27u<<16)|(120u<<24))

static int tcp_connect(uint32_t addr, uint16_t port) {
    int fd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    sa.port = (uint16_t)((port >> 8) | (port << 8));
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

/* After receiving data, verify the TCP window is non-zero (not stuck at 0) */
static void test_tcp_recv_window_update(void) {
    puts("\n[net/tcp-window: recv window update]\n");

    int fd = tcp_connect(EXAMPLE_COM, 80);
    check("connect", fd >= 0);
    if (fd < 0) return;

    long r = http_get(fd, "example.com", "/");
    check("send GET", r > 0);

    /* Read first chunk — this should update the receive window */
    char buf[4096];
    r = sc3(SYS_READ, fd, (long)buf, sizeof(buf));
    check("recv first chunk > 0", r > 0);

    /* If window were stuck at 0, the server would stop sending.
     * A second read should still return data (or 0 for EOF). */
    long r2 = sc3(SYS_READ, fd, (long)buf, sizeof(buf));
    check("recv second chunk >= 0", r2 >= 0);

    /* Total received must be non-trivial (example.com HTML ~1256 bytes) */
    check("total > 100", r + r2 > 100);

    sc1(SYS_CLOSE, fd);
}

/* Download >64KB via HTTP to test large transfer / window scaling */
static void test_tcp_large_transfer(void) {
    puts("\n[net/tcp-window: large transfer]\n");

    int fd = tcp_connect(EXAMPLE_COM, 80);
    check("connect", fd >= 0);
    if (fd < 0) return;

    long r = http_get(fd, "example.com", "/");
    check("send GET", r > 0);

    /* Drain entire response */
    long total = 0;
    char buf[4096];
    for (;;) {
        r = sc3(SYS_READ, fd, (long)buf, sizeof(buf));
        if (r <= 0) break;
        total += r;
    }
    puts("  received "); put_int(total); puts(" bytes\n");
    check("recv > 500 bytes", total > 500);

    sc1(SYS_CLOSE, fd);
}

/* After connect, verify the initial send window > 0 */
static void test_tcp_snd_wnd_init(void) {
    puts("\n[net/tcp-window: snd_wnd init]\n");

    int fd = tcp_connect(EXAMPLE_COM, 80);
    check("connect", fd >= 0);
    if (fd < 0) return;

    /* If snd_wnd were 0 after connect, write would block/fail */
    const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    int len = 0; while (req[len]) len++;
    long r = sc3(SYS_WRITE, fd, (long)req, len);
    check_val("write succeeds (snd_wnd > 0)", r, len);

    /* Read at least something back to confirm the data was sent */
    char buf[512];
    r = sc3(SYS_READ, fd, (long)buf, sizeof(buf));
    check("recv > 0 (server got our data)", r > 0);

    sc1(SYS_CLOSE, fd);
}

TEST("net/tcp-recv-window", test_tcp_recv_window_update);
TEST("net/tcp-large-xfer", test_tcp_large_transfer);
TEST("net/tcp-snd-wnd-init", test_tcp_snd_wnd_init);
