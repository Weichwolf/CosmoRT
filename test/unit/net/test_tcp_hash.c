/* test: TCP Hash-Table — multiple concurrent connections, collision resilience
 * Verifies that the chained hash-table correctly handles:
 *   - Multiple connections to the same server (same dst_ip:dport, different sport)
 *   - Unregister of one connection doesn't break others
 *   - 8+ simultaneous TCP connections without data loss
 */
#include "ktest.h"

/* example.com 104.18.27.120:80 — routed via QEMU slirp */
static void fill_addr(void *sa_out) {
    char *p = sa_out;
    for (int i = 0; i < 16; i++) p[i] = 0;
    uint16_t *fam  = (uint16_t *)p;
    uint16_t *port = (uint16_t *)(p + 2);
    uint32_t *addr = (uint32_t *)(p + 4);
    *fam  = 2; /* AF_INET */
    *port = (uint16_t)((80 >> 8) | (80 << 8));
    *addr = (uint32_t)104 | (18u << 8) | (27u << 16) | (120u << 24);
}

static const char *http_req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";

static int http_reqlen(void) {
    int n = 0; while (http_req[n]) n++;
    return n;
}

/* ── Test 1: Two connections, both findable ── */

static void test_tcp_hash_two(void) {
    puts("\n[net/tcp_hash_two]\n");

    char sa[16];
    fill_addr(sa);

    int fd1 = (int)sc3(SYS_SOCKET, 2, 1, 0);
    int fd2 = (int)sc3(SYS_SOCKET, 2, 1, 0);
    check("socket1", fd1 >= 0);
    check("socket2", fd2 >= 0);
    if (fd1 < 0 || fd2 < 0) goto cleanup2;

    long r1 = sc3(SYS_CONNECT, fd1, (long)sa, 16);
    long r2 = sc3(SYS_CONNECT, fd2, (long)sa, 16);
    check("connect1", r1 == 0);
    check("connect2", r2 == 0);
    if (r1 != 0 || r2 != 0) goto cleanup2;

    /* Send on both */
    int rlen = http_reqlen();
    r1 = sc3(SYS_WRITE, fd1, (long)http_req, rlen);
    r2 = sc3(SYS_WRITE, fd2, (long)http_req, rlen);
    check("send1", r1 == rlen);
    check("send2", r2 == rlen);

    /* Recv on both — both must get HTTP response */
    char buf[256];
    r1 = sc3(SYS_READ, fd1, (long)buf, sizeof(buf));
    check("recv1 > 0", r1 > 0);
    if (r1 >= 4)
        check("recv1 HTTP", buf[0]=='H' && buf[1]=='T' && buf[2]=='T' && buf[3]=='P');

    r2 = sc3(SYS_READ, fd2, (long)buf, sizeof(buf));
    check("recv2 > 0", r2 > 0);
    if (r2 >= 4)
        check("recv2 HTTP", buf[0]=='H' && buf[1]=='T' && buf[2]=='T' && buf[3]=='P');

cleanup2:
    if (fd1 >= 0) sc1(SYS_CLOSE, fd1);
    if (fd2 >= 0) sc1(SYS_CLOSE, fd2);
}

/* ── Test 2: Close one, other still works ── */

static void test_tcp_hash_unregister(void) {
    puts("\n[net/tcp_hash_unreg]\n");

    char sa[16];
    fill_addr(sa);

    int fd1 = (int)sc3(SYS_SOCKET, 2, 1, 0);
    int fd2 = (int)sc3(SYS_SOCKET, 2, 1, 0);
    check("socket1", fd1 >= 0);
    check("socket2", fd2 >= 0);
    if (fd1 < 0 || fd2 < 0) goto cleanup;

    long r = sc3(SYS_CONNECT, fd1, (long)sa, 16);
    check("connect1", r == 0);
    if (r != 0) goto cleanup;

    r = sc3(SYS_CONNECT, fd2, (long)sa, 16);
    check("connect2", r == 0);
    if (r != 0) goto cleanup;

    /* Close fd1 — removes from hash */
    sc1(SYS_CLOSE, fd1);
    fd1 = -1;

    /* fd2 must still work */
    int rlen = http_reqlen();
    r = sc3(SYS_WRITE, fd2, (long)http_req, rlen);
    check("send after close1", r == rlen);

    char buf[256];
    r = sc3(SYS_READ, fd2, (long)buf, sizeof(buf));
    check("recv after close1 > 0", r > 0);
    if (r >= 4)
        check("recv after close1 HTTP",
              buf[0]=='H' && buf[1]=='T' && buf[2]=='T' && buf[3]=='P');

cleanup:
    if (fd1 >= 0) sc1(SYS_CLOSE, fd1);
    if (fd2 >= 0) sc1(SYS_CLOSE, fd2);
}

/* ── Test 3: 8 simultaneous connections ── */

#define MULTI_N 8

static void test_tcp_hash_multi(void) {
    puts("\n[net/tcp_hash_multi]\n");

    char sa[16];
    fill_addr(sa);
    int fds[MULTI_N];
    int ok_count = 0;

    for (int i = 0; i < MULTI_N; i++) fds[i] = -1;

    /* Open and connect all */
    for (int i = 0; i < MULTI_N; i++) {
        fds[i] = (int)sc3(SYS_SOCKET, 2, 1, 0);
        if (fds[i] < 0) break;
        long r = sc3(SYS_CONNECT, fds[i], (long)sa, 16);
        if (r != 0) { sc1(SYS_CLOSE, fds[i]); fds[i] = -1; break; }
        ok_count++;
    }
    check("8 connects", ok_count == MULTI_N);

    /* Send HTTP GET on all */
    int rlen = http_reqlen();
    int send_ok = 0;
    for (int i = 0; i < ok_count; i++) {
        long r = sc3(SYS_WRITE, fds[i], (long)http_req, rlen);
        if (r == rlen) send_ok++;
    }
    check("8 sends", send_ok == ok_count);

    /* Recv on all — each must get HTTP response */
    int recv_ok = 0;
    for (int i = 0; i < ok_count; i++) {
        char buf[256];
        long r = sc3(SYS_READ, fds[i], (long)buf, sizeof(buf));
        if (r >= 4 && buf[0]=='H' && buf[1]=='T' && buf[2]=='T' && buf[3]=='P')
            recv_ok++;
    }
    check("8 recvs HTTP", recv_ok == ok_count);

    /* Cleanup */
    for (int i = 0; i < MULTI_N; i++)
        if (fds[i] >= 0) sc1(SYS_CLOSE, fds[i]);
}

TEST("net/tcp_hash_two", test_tcp_hash_two);
TEST("net/tcp_hash_unreg", test_tcp_hash_unregister);
TEST("net/tcp_hash_multi", test_tcp_hash_multi);
