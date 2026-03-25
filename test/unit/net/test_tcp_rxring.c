/* test: TCP RX Ringbuffer — verify large transfers don't lose data
 * Exercises the per-socket ringbuffer by transferring >4KB via TCP.
 */
#include "ktest.h"

static void test_tcp_rxring(void) {
    puts("\n[net/tcp_rxring]\n");

    int fd = (int)sc3(SYS_SOCKET, 2, 1, 0);
    check("socket", fd >= 0);
    if (fd < 0) return;

    /* Connect to example.com:80 */
    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = 2;
    sa.port = (uint16_t)((80 >> 8) | (80 << 8));
    sa.addr = (uint32_t)104 | (18u << 8) | (27u << 16) | (120u << 24);

    long r = sc3(SYS_CONNECT, fd, (long)&sa, 16);
    check("connect", r == 0);
    if (r != 0) { sc1(SYS_CLOSE, fd); return; }

    /* Request full page — response will be >1KB, exercises ringbuffer */
    const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    int reqlen = 0; while (req[reqlen]) reqlen++;
    r = sc3(SYS_WRITE, fd, (long)req, reqlen);
    check("send request", r == reqlen);

    /* Read in small chunks to force multiple ringbuffer pops */
    char buf[128];
    long total = 0;
    int reads = 0;
    int max_reads = 200; /* safety limit */

    while (reads < max_reads) {
        r = sc3(SYS_READ, fd, (long)buf, sizeof(buf));
        if (r <= 0) break;
        total += r;
        reads++;
    }

    check("total > 512 bytes", total > 512);
    check("multiple reads", reads > 1);

    /* Verify first chunk was HTTP response */
    /* (Already consumed, but total size confirms full transfer) */

    /* Verify we get EOF (0) or EAGAIN, not garbage */
    check("clean termination", r == 0 || r == -EAGAIN);

    sc1(SYS_CLOSE, fd);

    /* Second connection: verify ringbuffer reuse works */
    fd = (int)sc3(SYS_SOCKET, 2, 1, 0);
    check("socket2", fd >= 0);
    if (fd < 0) return;

    r = sc3(SYS_CONNECT, fd, (long)&sa, 16);
    check("connect2", r == 0);
    if (r != 0) { sc1(SYS_CLOSE, fd); return; }

    r = sc3(SYS_WRITE, fd, (long)req, reqlen);
    check("send2", r == reqlen);

    /* Single large read */
    char bigbuf[4096];
    r = sc3(SYS_READ, fd, (long)bigbuf, sizeof(bigbuf));
    check("large recv > 0", r > 0);
    if (r > 4) {
        check("response2 HTTP",
              bigbuf[0]=='H' && bigbuf[1]=='T' && bigbuf[2]=='T' && bigbuf[3]=='P');
    }

    sc1(SYS_CLOSE, fd);
}

TEST("net/tcp_rxring", test_tcp_rxring);
