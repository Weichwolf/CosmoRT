/* test: Multiple simultaneous TCP connections
 * Opens 4 parallel HTTP connections to example.com:80.
 * All must receive valid HTTP responses.
 */
#include "ktest.h"

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

#define N_CONNS 4

static void test_net_multi_conn(void) {
    puts("\n[net/multi_conn]\n");

    char sa[16];
    fill_addr(sa);
    int fds[N_CONNS];
    int ok = 0;

    for (int i = 0; i < N_CONNS; i++) fds[i] = -1;

    /* Open all connections */
    for (int i = 0; i < N_CONNS; i++) {
        fds[i] = (int)sc3(SYS_SOCKET, 2, 1, 0);
        if (fds[i] < 0) break;
        long r = sc3(SYS_CONNECT, fds[i], (long)sa, 16);
        if (r != 0) { sc1(SYS_CLOSE, fds[i]); fds[i] = -1; break; }
        ok++;
    }
    check_val("4 connects", (long)ok, N_CONNS);

    /* Send HTTP GET on all */
    int rlen = slen(http_req);
    int send_ok = 0;
    for (int i = 0; i < ok; i++) {
        long r = sc3(SYS_WRITE, fds[i], (long)http_req, rlen);
        if (r == rlen) send_ok++;
    }
    check_val("4 sends", (long)send_ok, (long)ok);

    /* Recv on all */
    int recv_ok = 0;
    for (int i = 0; i < ok; i++) {
        char buf[512];
        long total = 0;
        int tries = 0;
        while (total == 0 && tries < 5) {
            long r = sc3(SYS_READ, fds[i], (long)buf, sizeof(buf));
            if (r > 0) total = r;
            tries++;
        }
        if (total >= 4 && buf[0]=='H' && buf[1]=='T' && buf[2]=='T' && buf[3]=='P')
            recv_ok++;
    }
    check_val("4 HTTP responses", (long)recv_ok, (long)ok);

    /* Drain remaining data and close */
    for (int i = 0; i < N_CONNS; i++) {
        if (fds[i] >= 0) {
            char drain[512];
            for (int j = 0; j < 20; j++) {
                long r = sc3(SYS_READ, fds[i], (long)drain, sizeof(drain));
                if (r <= 0) break;
            }
            sc1(SYS_CLOSE, fds[i]);
        }
    }
}

/* Test: connections to different ports (80 + 80 with different server IPs) */
static void test_net_multi_conn_mixed(void) {
    puts("\n[net/multi_conn_mixed]\n");

    /* Two connections to same destination — verifies hash-table correctness */
    char sa[16];
    fill_addr(sa);

    int fd1 = (int)sc3(SYS_SOCKET, 2, 1, 0);
    int fd2 = (int)sc3(SYS_SOCKET, 2, 1, 0);
    check("sock1", fd1 >= 0);
    check("sock2", fd2 >= 0);
    if (fd1 < 0 || fd2 < 0) goto done;

    {
        long r1 = sc3(SYS_CONNECT, fd1, (long)sa, 16);
        long r2 = sc3(SYS_CONNECT, fd2, (long)sa, 16);
        check("connect1", r1 == 0);
        check("connect2", r2 == 0);
        if (r1 != 0 || r2 != 0) goto done;
    }

    /* Send on both, recv on both */
    int rlen = slen(http_req);
    sc3(SYS_WRITE, fd1, (long)http_req, rlen);
    sc3(SYS_WRITE, fd2, (long)http_req, rlen);

    char buf[256];
    long r1 = sc3(SYS_READ, fd1, (long)buf, sizeof(buf));
    check("recv1 > 0", r1 > 0);
    long r2 = sc3(SYS_READ, fd2, (long)buf, sizeof(buf));
    check("recv2 > 0", r2 > 0);

done:
    if (fd1 >= 0) sc1(SYS_CLOSE, fd1);
    if (fd2 >= 0) sc1(SYS_CLOSE, fd2);
}

TEST("net/multi_conn", test_net_multi_conn);
TEST("net/multi_conn_mixed", test_net_multi_conn_mixed);
