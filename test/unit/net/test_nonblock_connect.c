/* test: Non-blocking connect — EINPROGRESS, poll, getsockopt SO_ERROR */
#include "ktest.h"

#define EXAMPLE_COM ((uint32_t)104|(18u<<8)|(27u<<16)|(120u<<24))
#define SOCK_NONBLOCK 0x800

struct test_pollfd { int fd; short events; short revents; };
#define T_POLLIN  0x0001
#define T_POLLOUT 0x0004
#define T_POLLHUP 0x0010

static int make_nonblock_socket(void) {
    return (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
}

static void make_addr(void *sa, uint32_t addr, uint16_t port) {
    for (int i = 0; i < 16; i++) ((char *)sa)[i] = 0;
    ((uint16_t *)sa)[0] = AF_INET;
    ((uint16_t *)sa)[1] = (uint16_t)((port >> 8) | (port << 8));
    ((uint32_t *)sa)[1] = addr;
}

/* Non-blocking connect: EINPROGRESS → poll POLLOUT → getsockopt SO_ERROR=0 */
static void test_nonblock_connect(void) {
    puts("\n[net/nonblock: connect]\n");

    int fd = make_nonblock_socket();
    check("socket(NONBLOCK)", fd >= 0);
    if (fd < 0) return;

    char sa[16];
    make_addr(sa, EXAMPLE_COM, 80);
    long r = sc3(SYS_CONNECT, fd, (long)sa, 16);
    /* Must be either 0 (instant) or -EINPROGRESS */
    check("connect EINPROGRESS or 0", r == 0 || r == -EINPROGRESS);

    if (r == -EINPROGRESS) {
        /* Poll for completion */
        struct test_pollfd pfd = { fd, T_POLLOUT, 0 };
        long pr = sc3(SYS_POLL, (long)&pfd, 1, 5000);
        check("poll returns ready", pr >= 1);
        check("POLLOUT set", (pfd.revents & T_POLLOUT) != 0);
    }

    /* getsockopt SO_ERROR should be 0 */
    int err = -1;
    int errlen = 4;
    r = sc5(SYS_GETSOCKOPT, fd, SOL_SOCKET, SO_ERROR, (long)&err, (long)&errlen);
    check_val("getsockopt returns 0", r, 0);
    check_val("SO_ERROR = 0", err, 0);

    sc1(SYS_CLOSE, fd);
}

/* Non-blocking connect → poll → write succeeds */
static void test_nonblock_write_after_connect(void) {
    puts("\n[net/nonblock: write after connect]\n");

    int fd = make_nonblock_socket();
    check("socket", fd >= 0);
    if (fd < 0) return;

    char sa[16];
    make_addr(sa, EXAMPLE_COM, 80);
    long r = sc3(SYS_CONNECT, fd, (long)sa, 16);

    if (r == -EINPROGRESS) {
        struct test_pollfd pfd = { fd, T_POLLOUT, 0 };
        sc3(SYS_POLL, (long)&pfd, 1, 5000);
    }

    const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    int len = 0; while (req[len]) len++;
    r = sc3(SYS_WRITE, fd, (long)req, len);
    check_val("write after connect", r, len);

    sc1(SYS_CLOSE, fd);
}

/* Non-blocking connect → send data → read returns response */
static void test_nonblock_read_after_connect(void) {
    puts("\n[net/nonblock: read after connect]\n");

    int fd = make_nonblock_socket();
    check("socket", fd >= 0);
    if (fd < 0) return;

    char sa[16];
    make_addr(sa, EXAMPLE_COM, 80);
    long r = sc3(SYS_CONNECT, fd, (long)sa, 16);
    check("connect ok or EINPROGRESS", r == 0 || r == -EINPROGRESS);
    if (r != 0 && r != -EINPROGRESS) { sc1(SYS_CLOSE, fd); return; }

    if (r == -EINPROGRESS) {
        struct test_pollfd pfd = { fd, T_POLLOUT, 0 };
        long pr = sc3(SYS_POLL, (long)&pfd, 1, 5000);
        check("poll connect complete", pr >= 1);
        if (pr < 1) { sc1(SYS_CLOSE, fd); return; }
    }

    const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    int len = 0; while (req[len]) len++;
    long wr = sc3(SYS_WRITE, fd, (long)req, len);
    check("write request", wr == len);
    if (wr != len) { sc1(SYS_CLOSE, fd); return; }

    /* Switch to blocking mode for reliable read */
    long fl = sc3(SYS_FCNTL, fd, F_GETFL, 0);
    sc3(SYS_FCNTL, fd, F_SETFL, fl & ~O_NONBLOCK);

    /* Poll for readable data */
    struct test_pollfd rpfd = { fd, T_POLLIN, 0 };
    long pr = sc3(SYS_POLL, (long)&rpfd, 1, 5000);
    check("poll readable", pr >= 1);

    char buf[512];
    r = sc3(SYS_READ, fd, (long)buf, sizeof(buf));
    check("read returns data", r > 0);
    if (r > 4) {
        check("response is HTTP", buf[0]=='H' && buf[1]=='T' && buf[2]=='T' && buf[3]=='P');
    }

    sc1(SYS_CLOSE, fd);
}

/* Write on unconnected socket → ENOTCONN */
static void test_unconnected_write(void) {
    puts("\n[net/nonblock: unconnected write]\n");

    int fd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("socket", fd >= 0);
    if (fd < 0) return;

    long r = sc3(SYS_WRITE, fd, (long)"x", 1);
    check_val("write unconnected → -ENOTCONN", r, -ENOTCONN);

    sc1(SYS_CLOSE, fd);
}

/* Read on unconnected socket → ENOTCONN */
static void test_unconnected_read(void) {
    puts("\n[net/nonblock: unconnected read]\n");

    int fd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("socket", fd >= 0);
    if (fd < 0) return;

    char buf[4];
    long r = sc3(SYS_READ, fd, (long)buf, sizeof(buf));
    check_val("read unconnected → -ENOTCONN", r, -ENOTCONN);

    sc1(SYS_CLOSE, fd);
}

TEST("net/nonblock-connect", test_nonblock_connect);
TEST("net/nonblock-write", test_nonblock_write_after_connect);
TEST("net/nonblock-read", test_nonblock_read_after_connect);
TEST("net/unconnected-write", test_unconnected_write);
TEST("net/unconnected-read", test_unconnected_read);
