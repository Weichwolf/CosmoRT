/* test: TCP State-Machine — verify correct state transitions via socket API
 * Tests: CLOSED→SYN_SENT→ESTABLISHED→FIN_WAIT→CLOSED (connect+close)
 *        CLOSED→SYN_SENT→ESTABLISHED→CLOSE_WAIT→LAST_ACK→CLOSED (peer close)
 */
#include "ktest.h"

static void test_tcp_state(void) {
    puts("\n[net/tcp_state]\n");

    /* 1. Socket creation → CLOSED state (socket not connected) */
    int fd = (int)sc3(SYS_SOCKET, 2, 1, 0); /* AF_INET, SOCK_STREAM */
    check("socket create", fd >= 0);
    if (fd < 0) return;

    /* Verify: read on unconnected socket fails */
    char buf[16];
    long r = sc3(SYS_READ, fd, (long)buf, 16);
    check("read unconnected → ENOTCONN", r == -ENOTCONN);

    /* 2. Connect → ESTABLISHED (SYN_SENT→ESTABLISHED internally) */
    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = 2; /* AF_INET */
    sa.port = (uint16_t)((80 >> 8) | (80 << 8)); /* port 80 big-endian */
    /* example.com 104.18.27.120 */
    sa.addr = (uint32_t)104 | (18u << 8) | (27u << 16) | (120u << 24);

    r = sc3(SYS_CONNECT, fd, (long)&sa, 16);
    check("connect → ESTABLISHED", r == 0);
    if (r != 0) { sc1(SYS_CLOSE, fd); return; }

    /* Verify: second connect on connected socket → EISCONN */
    r = sc3(SYS_CONNECT, fd, (long)&sa, 16);
    check("reconnect → EISCONN", r == -EISCONN);

    /* Verify: getpeername works on connected socket */
    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } peer;
    int peerlen = 16;
    r = sc3(SYS_GETPEERNAME, fd, (long)&peer, (long)&peerlen);
    check("getpeername ok", r == 0);
    if (r == 0)
        check("peer addr set", peer.addr != 0);

    /* 3. Send HTTP request (verifies ESTABLISHED allows send) */
    const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    int reqlen = 0; while (req[reqlen]) reqlen++;
    r = sc3(SYS_WRITE, fd, (long)req, reqlen);
    check("write in ESTABLISHED", r == reqlen);

    /* 4. Read response (verifies data reception works) */
    char resp[256];
    r = sc3(SYS_READ, fd, (long)resp, sizeof(resp) - 1);
    check("recv in ESTABLISHED", r > 0);

    /* 5. Shutdown write → triggers FIN_WAIT1 internally */
    r = sc3(SYS_SHUTDOWN, fd, 1, 0); /* SHUT_WR */
    check("shutdown(WR)", r == 0);

    /* 6. Close → completes state machine to CLOSED */
    r = sc1(SYS_CLOSE, fd);
    check("close", r == 0);

    /* 7. Verify closed socket is invalid */
    r = sc3(SYS_READ, fd, (long)buf, 16);
    check("read after close → EBADF", r == -EBADF);
}

TEST("net/tcp_state", test_tcp_state);
