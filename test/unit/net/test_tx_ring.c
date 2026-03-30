/* test: TX Ring — verify Compute→RT TX path works for network sends.
 * Exercises the TX ring by sending TCP data from a Compute core.
 * The send goes: send() syscall → ip_send_raw() → tx_ring_push() →
 * net_poll() on RT-Core → tx_ring_drain() → nic->send().
 */
#include "ktest.h"
#include "cosmort.h"

static void test_tx_ring(void) {
    puts("\n[net/tx_ring]\n");

    /* ktest runs in userspace (always on non-BSP core) */

    /* TCP connect exercises the TX ring: SYN is built on Compute core,
     * pushed into TX ring, drained by RT-Core's net_poll(), sent via NIC.
     * If TX ring is broken, connect will timeout. */
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
    check("connect via tx_ring", r == 0);
    if (r != 0) { sc1(SYS_CLOSE, fd); return; }

    /* Send HTTP request — multiple segments exercise TX ring throughput */
    const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    int reqlen = 0; while (req[reqlen]) reqlen++;
    r = sc3(SYS_WRITE, fd, (long)req, reqlen);
    check("send via tx_ring", r == reqlen);

    /* Read response — proves the full TX→RX roundtrip worked */
    char buf[256];
    r = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    check("recv response", r > 0);
    if (r > 4) {
        buf[r] = 0;
        check("response is HTTP",
              buf[0]=='H' && buf[1]=='T' && buf[2]=='T' && buf[3]=='P');
    }

    sc1(SYS_CLOSE, fd);
}

TEST("net/tx_ring", test_tx_ring);
