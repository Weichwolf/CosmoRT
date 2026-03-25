/* test: TCP — connect + HTTP GET against example.com */
#include "ktest.h"

static void test_tcp_connect(void) {
    puts("\n[net/tcp]\n");

    int fd = (int)sc3(SYS_SOCKET, 2, 1, 0);
    check("socket", fd >= 0);
    if (fd < 0) return;

    /* example.com 104.18.27.120:80 */
    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = 2;
    sa.port = (uint16_t)((80 >> 8) | (80 << 8));
    sa.addr = (uint32_t)104 | (18u << 8) | (27u << 16) | (120u << 24);

    long r = sc3(SYS_CONNECT, fd, (long)&sa, 16);
    check("connect example.com:80", r == 0);
    if (r != 0) { sc1(SYS_CLOSE, fd); return; }

    /* HTTP GET */
    const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    int reqlen = 0; while (req[reqlen]) reqlen++;
    r = sc3(SYS_WRITE, fd, (long)req, reqlen);
    check("send HTTP GET", r == reqlen);

    /* Read response */
    char buf[512];
    r = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    check("recv > 0", r > 0);
    if (r > 4) {
        buf[r] = 0;
        check("response HTTP/1.",
              buf[0]=='H' && buf[1]=='T' && buf[2]=='T' && buf[3]=='P');
        /* Check for 200 status */
        int has_200 = 0;
        for (int i = 0; i < r - 2; i++)
            if (buf[i]=='2' && buf[i+1]=='0' && buf[i+2]=='0') has_200 = 1;
        check("status 200", has_200);
    }

    sc1(SYS_CLOSE, fd);
}

TEST("net/tcp", test_tcp_connect);
