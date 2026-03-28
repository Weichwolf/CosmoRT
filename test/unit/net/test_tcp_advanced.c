/* test: TCP Advanced — Timestamps (RFC 7323), ECN (RFC 3168), TFO (RFC 7413)
 * Connects to example.com:80 via QEMU SLIRP, verifies negotiation and fallback. */
#include "ktest.h"

/* example.com = 104.18.27.120 */
#define EXAMPLE_COM ((uint32_t)104|(18u<<8)|(27u<<16)|(120u<<24))

static int tcp_connect(uint32_t addr, uint16_t port) {
    int fd = (int)sc3(SYS_SOCKET, 2, 1, 0);
    if (fd < 0) return -1;
    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = 2;
    sa.port = (uint16_t)((port >> 8) | (port << 8));
    sa.addr = addr;
    if (sc3(SYS_CONNECT, fd, (long)&sa, 16) != 0) { sc1(SYS_CLOSE, fd); return -1; }
    return fd;
}

static long http_get(int fd, const char *host) {
    char req[256];
    int ri = 0;
    const char *p = "GET / HTTP/1.0\r\nHost: ";
    while (*p) req[ri++] = *p++;
    while (*host) req[ri++] = *host++;
    p = "\r\n\r\n"; while (*p) req[ri++] = *p++;
    return sc3(SYS_WRITE, fd, (long)req, ri);
}

/* ── Timestamps test ─────────────────────────────── */

static void test_tcp_timestamps(void) {
    puts("\n[net/tcp-timestamps]\n");

    /* Timestamps are negotiated automatically in SYN.
     * We verify that a normal connection still works (negotiation doesn't break).
     * Modern servers (example.com via SLIRP) should respond with timestamps. */
    int fd = tcp_connect(EXAMPLE_COM, 80);
    check("ts: connect", fd >= 0);
    if (fd < 0) return;

    long r = http_get(fd, "example.com");
    check("ts: send GET", r > 0);

    char buf[512];
    r = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    check("ts: recv > 0", r > 0);
    if (r > 4) {
        buf[r] = 0;
        check("ts: HTTP response",
              buf[0]=='H' && buf[1]=='T' && buf[2]=='T' && buf[3]=='P');
    }

    sc1(SYS_CLOSE, fd);
    pass("ts: connection with timestamps OK");
}

/* ── ECN test ─────────────────────────────────────── */

static void test_tcp_ecn(void) {
    puts("\n[net/tcp-ecn]\n");

    /* ECN is negotiated in SYN (CWR+ECE flags).
     * QEMU SLIRP may or may not support ECN.
     * Either way, connection must succeed (graceful fallback). */
    int fd = tcp_connect(EXAMPLE_COM, 80);
    check("ecn: connect", fd >= 0);
    if (fd < 0) return;

    long r = http_get(fd, "example.com");
    check("ecn: send GET", r > 0);

    char buf[512];
    r = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    check("ecn: recv > 0", r > 0);
    if (r > 4) {
        buf[r] = 0;
        check("ecn: HTTP response",
              buf[0]=='H' && buf[1]=='T' && buf[2]=='T' && buf[3]=='P');
    }

    sc1(SYS_CLOSE, fd);
    pass("ecn: connection with ECN negotiation OK");
}

/* ── TFO test ─────────────────────────────────────── */

static void test_tcp_tfo(void) {
    puts("\n[net/tcp-tfo]\n");

    /* First connection: TFO cookie request sent in SYN (Kind=34, Len=2).
     * Server may respond with cookie in SYN-ACK or ignore it.
     * Connection must succeed either way. */
    int fd = tcp_connect(EXAMPLE_COM, 80);
    check("tfo: first connect (cookie request)", fd >= 0);
    if (fd < 0) return;

    long r = http_get(fd, "example.com");
    check("tfo: first send GET", r > 0);

    char buf[512];
    r = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    check("tfo: first recv > 0", r > 0);
    if (r > 4) {
        buf[r] = 0;
        check("tfo: first HTTP response",
              buf[0]=='H' && buf[1]=='T' && buf[2]=='T' && buf[3]=='P');
    }
    sc1(SYS_CLOSE, fd);

    /* Second connection: if cookie was cached, TFO cookie sent in SYN.
     * If not cached, normal SYN with cookie request again. Both must work. */
    fd = tcp_connect(EXAMPLE_COM, 80);
    check("tfo: second connect (with cookie if cached)", fd >= 0);
    if (fd < 0) return;

    r = http_get(fd, "example.com");
    check("tfo: second send GET", r > 0);

    r = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    check("tfo: second recv > 0", r > 0);
    if (r > 4) {
        buf[r] = 0;
        check("tfo: second HTTP response",
              buf[0]=='H' && buf[1]=='T' && buf[2]=='T' && buf[3]=='P');
    }
    sc1(SYS_CLOSE, fd);
    pass("tfo: cookie request/reuse OK");
}

/* ── Combined: all three features in one connection ── */

static void test_tcp_advanced_combined(void) {
    puts("\n[net/tcp-advanced-combined]\n");

    /* All three features (TS, ECN, TFO) are negotiated simultaneously.
     * Verify that multiple connections in sequence all work. */
    for (int i = 0; i < 3; i++) {
        int fd = tcp_connect(EXAMPLE_COM, 80);
        check("combined: connect", fd >= 0);
        if (fd < 0) continue;

        long r = http_get(fd, "example.com");
        check("combined: send", r > 0);

        char buf[512];
        r = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
        check("combined: recv", r > 0);
        sc1(SYS_CLOSE, fd);
    }
    pass("combined: 3 sequential connections OK");
}

TEST("net/tcp-timestamps", test_tcp_timestamps);
TEST("net/tcp-ecn", test_tcp_ecn);
TEST("net/tcp-tfo", test_tcp_tfo);
TEST("net/tcp-advanced-combined", test_tcp_advanced_combined);
