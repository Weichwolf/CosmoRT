/* test: Socket slab pool — scale, recycle, reuse */
#include "ktest.h"

static void test_socket_scale(void) {
    puts("\n[socket/scale]\n");

    /* 1. Open 128 TCP sockets */
    int fds[128];
    int opened = 0;
    for (int i = 0; i < 128; i++) {
        long fd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
        if (fd < 0) break;
        fds[i] = (int)fd;
        opened++;
    }
    check_ge("open 128 sockets", opened, 128);

    /* 2. Close all */
    for (int i = 0; i < opened; i++)
        sc1(SYS_CLOSE, fds[i]);

    /* 3. Re-allocate after close (slab recycles) */
    int reopened = 0;
    for (int i = 0; i < 128; i++) {
        long fd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
        if (fd < 0) break;
        fds[i] = (int)fd;
        reopened++;
    }
    check_ge("re-open 128 after close", reopened, 128);

    /* 4. Close again */
    for (int i = 0; i < reopened; i++)
        sc1(SYS_CLOSE, fds[i]);

    /* 5. Alloc/free roundtrip — single socket */
    long fd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("single alloc", fd >= 0);
    if (fd >= 0) {
        long r = sc1(SYS_CLOSE, fd);
        check_val("single free", r, 0);
    }

    /* 6. UDP sockets too */
    int udp_opened = 0;
    for (int i = 0; i < 64; i++) {
        long ufd = sc3(SYS_SOCKET, AF_INET, SOCK_DGRAM, 0);
        if (ufd < 0) break;
        fds[i] = (int)ufd;
        udp_opened++;
    }
    check_ge("open 64 UDP sockets", udp_opened, 64);
    for (int i = 0; i < udp_opened; i++)
        sc1(SYS_CLOSE, fds[i]);
}
TEST("socket/scale", test_socket_scale);
