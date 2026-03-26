/* test: TCP OOO Buffering — verify out-of-order segment handling
 * Tests the OOO buffer logic via real TCP connections.
 * Indirect: OOO is tested by transferring enough data that reordering
 * may occur at the network level. Direct unit test of the buffer API
 * would require kernel-internal access (not available in userspace tests).
 *
 * Strategy: large HTTP GET exercises the receive path extensively.
 * If OOO buffering is broken, data would be silently dropped (short read).
 */
#include "ktest.h"

/* Connect to example.com:80 and GET a page */
static void fill_addr(void *sa_out) {
    char *p = sa_out;
    for (int i = 0; i < 16; i++) p[i] = 0;
    uint16_t *fam  = (uint16_t *)p;
    uint16_t *port = (uint16_t *)(p + 2);
    uint32_t *addr = (uint32_t *)(p + 4);
    *fam  = 2;
    *port = (uint16_t)((80 >> 8) | (80 << 8));
    *addr = (uint32_t)104 | (18u << 8) | (27u << 16) | (120u << 24);
}

static const char *http_req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* Test 1: Large transfer — reads entire HTTP response in small chunks.
 * Exercises OOO path if any segments arrive out of order. */
static void test_tcp_ooo_large_transfer(void) {
    puts("\n[net/tcp_ooo_large]\n");

    char sa[16];
    fill_addr(sa);

    int fd = (int)sc3(SYS_SOCKET, 2, 1, 0);
    check("socket", fd >= 0);
    if (fd < 0) return;

    long r = sc3(SYS_CONNECT, fd, (long)sa, 16);
    check("connect", r == 0);
    if (r != 0) { sc1(SYS_CLOSE, fd); return; }

    int rlen = slen(http_req);
    r = sc3(SYS_WRITE, fd, (long)http_req, rlen);
    check("send", r == rlen);

    /* Read in very small chunks (64 bytes) to maximize ringbuffer churn */
    char buf[64];
    long total = 0;
    int reads = 0;
    while (reads < 500) {
        r = sc3(SYS_READ, fd, (long)buf, sizeof(buf));
        if (r <= 0) break;
        total += r;
        reads++;
    }

    check("total > 512", total > 512);
    check("multiple reads", reads > 4);
    check("clean EOF", r == 0 || r == -EAGAIN);

    sc1(SYS_CLOSE, fd);
}

/* Test 2: Two sequential connections — OOO state must reset cleanly */
static void test_tcp_ooo_reset(void) {
    puts("\n[net/tcp_ooo_reset]\n");

    char sa[16];
    fill_addr(sa);

    for (int iter = 0; iter < 2; iter++) {
        int fd = (int)sc3(SYS_SOCKET, 2, 1, 0);
        check("socket", fd >= 0);
        if (fd < 0) return;

        long r = sc3(SYS_CONNECT, fd, (long)sa, 16);
        check("connect", r == 0);
        if (r != 0) { sc1(SYS_CLOSE, fd); return; }

        int rlen = slen(http_req);
        r = sc3(SYS_WRITE, fd, (long)http_req, rlen);
        check("send", r == rlen);

        char buf[512];
        r = sc3(SYS_READ, fd, (long)buf, sizeof(buf));
        check("recv > 0", r > 0);
        if (r >= 4)
            check("HTTP response", buf[0]=='H' && buf[1]=='T' && buf[2]=='T' && buf[3]=='P');

        sc1(SYS_CLOSE, fd);
    }
}

TEST("net/tcp_ooo_large", test_tcp_ooo_large_transfer);
TEST("net/tcp_ooo_reset", test_tcp_ooo_reset);
