#include "ktest.h"

static void test_tcp_server(void) {
    puts("\n[TCP server]\n");

    /* socket(AF_INET, SOCK_STREAM, 0) */
    long fd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("socket(AF_INET) >= 0", fd >= 0);
    if (fd < 0) return;

    /* setsockopt SO_REUSEADDR */
    int one = 1;
    long r = sc5(SYS_SETSOCKOPT, fd, SOL_SOCKET, SO_REUSEADDR, (long)&one, (long)sizeof(int));
    check_val("setsockopt SO_REUSEADDR", r, 0);

    /* bind to 127.0.0.1:8080 */
    struct {
        uint16_t sin_family;
        uint16_t sin_port;
        uint32_t sin_addr;
        uint8_t  sin_zero[8];
    } addr;
    for (int i = 0; i < (int)sizeof(addr); i++) ((char *)&addr)[i] = 0;
    addr.sin_family = AF_INET;
    addr.sin_port = (uint16_t)((8080 >> 8) | ((8080 & 0xFF) << 8)); /* htons(8080) */
    addr.sin_addr = (uint32_t)(127 | (0 << 8) | (0 << 16) | (1 << 24)); /* 127.0.0.1 LE */

    r = sc3(SYS_BIND, fd, (long)&addr, (long)sizeof(addr));
    check_val("bind 127.0.0.1:8080", r, 0);

    /* listen */
    r = sc2(SYS_LISTEN, fd, 5);
    check_val("listen(fd, 5)", r, 0);

    /* getsockname — verify bound address */
    struct {
        uint16_t sin_family;
        uint16_t sin_port;
        uint32_t sin_addr;
        uint8_t  sin_zero[8];
    } got_addr;
    for (int i = 0; i < (int)sizeof(got_addr); i++) ((char *)&got_addr)[i] = 0;
    int got_len = 0;
    r = sc3(SYS_GETSOCKNAME, fd, (long)&got_addr, (long)&got_len);
    check_val("getsockname returns 0", r, 0);
    check_val("getsockname family", (long)got_addr.sin_family, AF_INET);
    check_val("getsockname port", (long)got_addr.sin_port, (long)addr.sin_port);
    check_val("getsockname addr", (long)got_addr.sin_addr, (long)addr.sin_addr);
    check_val("getsockname addrlen", (long)got_len, 16);

    /* setsockopt TCP_NODELAY */
    r = sc5(SYS_SETSOCKOPT, fd, IPPROTO_TCP, TCP_NODELAY, (long)&one, (long)sizeof(int));
    check_val("setsockopt TCP_NODELAY", r, 0);

    /* getsockopt TCP_NODELAY */
    int got_val = 0;
    int got_optlen = 0;
    r = sc5(SYS_GETSOCKOPT, fd, IPPROTO_TCP, TCP_NODELAY, (long)&got_val, (long)&got_optlen);
    check_val("getsockopt TCP_NODELAY ret", r, 0);
    check_val("getsockopt TCP_NODELAY val", (long)got_val, 1);

    /* setsockopt/getsockopt SO_KEEPALIVE */
    r = sc5(SYS_SETSOCKOPT, fd, SOL_SOCKET, SO_KEEPALIVE, (long)&one, (long)sizeof(int));
    check_val("setsockopt SO_KEEPALIVE", r, 0);
    got_val = 0;
    r = sc5(SYS_GETSOCKOPT, fd, SOL_SOCKET, SO_KEEPALIVE, (long)&got_val, (long)&got_optlen);
    check_val("getsockopt SO_KEEPALIVE ret", r, 0);
    check_val("getsockopt SO_KEEPALIVE val", (long)got_val, 1);

    /* accept on empty queue (non-blocking) -> EAGAIN */
    sc3(SYS_FCNTL, fd, F_SETFL, O_NONBLOCK);
    r = sc3(SYS_ACCEPT, fd, 0, 0);
    check_val("accept empty -> EAGAIN", r, -EAGAIN);
    sc3(SYS_FCNTL, fd, F_SETFL, 0);

    /* shutdown */
    r = sc2(SYS_SHUTDOWN, fd, SHUT_RDWR);
    check_val("shutdown RDWR", r, 0);

    /* close */
    r = sc1(SYS_CLOSE, fd);
    check_val("close", r, 0);

    /* ── Edge cases ── */

    /* listen without bind -> EDESTADDRREQ */
    fd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("socket2 >= 0", fd >= 0);
    if (fd >= 0) {
        r = sc2(SYS_LISTEN, fd, 5);
        check_val("listen no bind -> EDESTADDRREQ", r, -EDESTADDRREQ);
        sc1(SYS_CLOSE, fd);
    }

    /* bind duplicate port without SO_REUSEADDR -> EADDRINUSE */
    long fd1 = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    long fd2 = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    if (fd1 >= 0 && fd2 >= 0) {
        struct {
            uint16_t sin_family;
            uint16_t sin_port;
            uint32_t sin_addr;
            uint8_t  sin_zero[8];
        } a2;
        for (int i = 0; i < (int)sizeof(a2); i++) ((char *)&a2)[i] = 0;
        a2.sin_family = AF_INET;
        a2.sin_port = (uint16_t)((9090 >> 8) | ((9090 & 0xFF) << 8));
        a2.sin_addr = 0; /* INADDR_ANY */

        r = sc3(SYS_BIND, fd1, (long)&a2, (long)sizeof(a2));
        check_val("bind1 port 9090", r, 0);
        r = sc3(SYS_BIND, fd2, (long)&a2, (long)sizeof(a2));
        check_val("bind2 port 9090 -> EADDRINUSE", r, -EADDRINUSE);

        sc1(SYS_CLOSE, fd1);
        sc1(SYS_CLOSE, fd2);
    }

    /* sendto on unconnected socket -> EBADF (not SOCK_CONNECTED) */
    fd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        char buf[4096];
        for (int i = 0; i < 4096; i++) buf[i] = (char)(i & 0xFF);
        r = sc6(SYS_SENDTO, fd, (long)buf, 4096, 0, 0, 0);
        check_val("sendto unconnected -> ENOTCONN", r, -ENOTCONN);
        sc1(SYS_CLOSE, fd);
    }

    /* Send >1500 bytes: full test requires connected socket + network.
     * Testable manually via: qemu -device e1000 -netdev user,hostfwd=tcp::8080-:8080 */
}
TEST("tcp_server", test_tcp_server);
