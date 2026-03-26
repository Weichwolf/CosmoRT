/* test: UDP Hash-Table Scale — bind 32+ sockets, verify find/unbind/collision.
 * Pure socket lifecycle, no network I/O needed.
 */
#include "ktest.h"

static void make_bind_addr(void *sa_out, uint16_t port) {
    char *p = sa_out;
    for (int i = 0; i < 16; i++) p[i] = 0;
    *(uint16_t *)p       = 2; /* AF_INET */
    *(uint16_t *)(p + 2) = (uint16_t)((port >> 8) | ((port & 0xFF) << 8));
    *(uint32_t *)(p + 4) = 0; /* INADDR_ANY */
}

/* ── Test 1: 32 simultaneous UDP sockets ── */

#define SCALE_N 32

static void test_udp_scale_bind(void) {
    puts("\n[net/udp_scale_bind]\n");

    int fds[SCALE_N];
    int ok = 0;

    for (int i = 0; i < SCALE_N; i++) fds[i] = -1;

    for (int i = 0; i < SCALE_N; i++) {
        fds[i] = (int)sc3(SYS_SOCKET, 2, 2, 0); /* AF_INET, SOCK_DGRAM */
        if (fds[i] < 0) break;

        char sa[16];
        make_bind_addr(sa, (uint16_t)(20000 + i));
        long r = sc3(SYS_BIND, fds[i], (long)sa, 16);
        if (r == 0) ok++;
    }
    check("32 UDP binds", ok == SCALE_N);

    for (int i = 0; i < SCALE_N; i++)
        if (fds[i] >= 0) sc1(SYS_CLOSE, fds[i]);
}

/* ── Test 2: unbind + rebind same port ── */

static void test_udp_rebind(void) {
    puts("\n[net/udp_rebind]\n");

    char sa[16];
    make_bind_addr(sa, 21000);

    int fd1 = (int)sc3(SYS_SOCKET, 2, 2, 0);
    check("socket1", fd1 >= 0);
    if (fd1 < 0) return;

    long r = sc3(SYS_BIND, fd1, (long)sa, 16);
    check("bind 21000", r == 0);
    sc1(SYS_CLOSE, fd1);

    /* Rebind same port */
    int fd2 = (int)sc3(SYS_SOCKET, 2, 2, 0);
    check("socket2", fd2 >= 0);
    if (fd2 < 0) return;

    r = sc3(SYS_BIND, fd2, (long)sa, 16);
    check("rebind 21000", r == 0);
    sc1(SYS_CLOSE, fd2);
}

/* ── Test 3: hash collision — ports that map to same bucket ── */

static void test_udp_collision(void) {
    puts("\n[net/udp_collision]\n");

    /* UDP_HASH_SIZE = 64, so port & 63 gives bucket.
     * Ports 22000 and 22064 both hash to bucket (22000 & 63) = 48. */
    uint16_t p1 = 22000;       /* bucket 48 */
    uint16_t p2 = (uint16_t)(p1 + 64);  /* same bucket */

    char sa1[16], sa2[16];
    make_bind_addr(sa1, p1);
    make_bind_addr(sa2, p2);

    int fd1 = (int)sc3(SYS_SOCKET, 2, 2, 0);
    int fd2 = (int)sc3(SYS_SOCKET, 2, 2, 0);
    check("collision sock1", fd1 >= 0);
    check("collision sock2", fd2 >= 0);
    if (fd1 < 0 || fd2 < 0) goto cleanup;

    long r1 = sc3(SYS_BIND, fd1, (long)sa1, 16);
    long r2 = sc3(SYS_BIND, fd2, (long)sa2, 16);
    check("bind collision p1", r1 == 0);
    check("bind collision p2", r2 == 0);

    /* Close first, second must survive */
    sc1(SYS_CLOSE, fd1);
    fd1 = -1;

    /* Verify fd2 still usable by binding another on p1 */
    int fd3 = (int)sc3(SYS_SOCKET, 2, 2, 0);
    check("collision sock3", fd3 >= 0);
    if (fd3 >= 0) {
        long r3 = sc3(SYS_BIND, fd3, (long)sa1, 16);
        check("rebind after collision", r3 == 0);
        sc1(SYS_CLOSE, fd3);
    }

cleanup:
    if (fd1 >= 0) sc1(SYS_CLOSE, fd1);
    if (fd2 >= 0) sc1(SYS_CLOSE, fd2);
}

TEST("net/udp_scale_bind", test_udp_scale_bind);
TEST("net/udp_rebind", test_udp_rebind);
TEST("net/udp_collision", test_udp_collision);
