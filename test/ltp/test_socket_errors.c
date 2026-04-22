/* LTP bind01/connect01/accept03 extra error-path coverage.
 *
 * Exercises exact errno paths LTP verifies but which were not in the
 * existing test_bind/test_connect/test_accept files:
 *   - bind01: EINVAL (salen<16), EAFNOSUPPORT (UNIX addr on INET sock),
 *             EADDRNOTAVAIL (non-local IP), ENOTDIR (AF_UNIX pathname
 *             with non-dir prefix).
 *   - connect01: EFAULT (bad pointer), EINVAL (salen<16), EAFNOSUPPORT.
 *   - accept03: EBADF on O_PATH fd.
 *   - AF_UNIX abstract namespace bind+connect+accept roundtrip. */
#include "ktest.h"

#define SOCK_CLOEXEC    0x80000
#define SOCK_NONBLOCK   0x800
#define O_PATH          010000000

/* ── bind01 — salen too small → EINVAL ── */
static void test_bind01_einval_salen(void) {
    puts("\n[ltp/bind01-einval-salen]\n");

    long sfd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("socket", sfd >= 0);
    if (sfd < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    long r = sc3(SYS_BIND, sfd, (long)&sa, 3);
    check_val("bind salen=3 EINVAL", r, -EINVAL);

    sc1(SYS_CLOSE, sfd);
}

/* ── bind01 — UNIX address on INET socket → EAFNOSUPPORT ── */
static void test_bind01_eafnosupport(void) {
    puts("\n[ltp/bind01-eafnosupport]\n");

    long sfd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("socket", sfd >= 0);
    if (sfd < 0) return;

    /* sockaddr_un, sun_family=AF_UNIX */
    struct { uint16_t family; char path[108]; } sun;
    for (int i = 0; i < 110; i++) ((char *)&sun)[i] = 0;
    sun.family = AF_UNIX;
    sun.path[0] = '.';

    long r = sc3(SYS_BIND, sfd, (long)&sun, 110);
    check_val("bind UNIX on INET sock EAFNOSUPPORT", r, -EAFNOSUPPORT);

    sc1(SYS_CLOSE, sfd);
}

/* ── bind01 — non-local address → EADDRNOTAVAIL ── */
static void test_bind01_eaddrnotavail(void) {
    puts("\n[ltp/bind01-eaddrnotavail]\n");

    long sfd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("socket", sfd >= 0);
    if (sfd < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    sa.port = 0;
    /* sin_addr is stored big-endian network-order bytes; on LE the bytes
     * {10,255,254,253} (IP 10.255.254.253) pack to value 0xFDFEFF0A. */
    sa.addr = 0xFDFEFF0A;

    long r = sc3(SYS_BIND, sfd, (long)&sa, 16);
    check_val("bind non-local EADDRNOTAVAIL", r, -EADDRNOTAVAIL);

    sc1(SYS_CLOSE, sfd);
}

/* ── bind01 — AF_UNIX pathname with non-dir prefix → ENOTDIR ── */
static void test_bind01_enotdir(void) {
    puts("\n[ltp/bind01-enotdir]\n");

    /* Create a regular file */
    long fd = sc3(SYS_OPEN, (long)"/tmp/enotdir_file", O_RDWR | O_CREAT, 0644);
    check("open /tmp/enotdir_file", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    long sfd = sc3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
    check("unix socket", sfd >= 0);
    if (sfd < 0) { sc1(SYS_UNLINK, (long)"/tmp/enotdir_file"); return; }

    struct { uint16_t family; char path[108]; } sun;
    for (int i = 0; i < 110; i++) ((char *)&sun)[i] = 0;
    sun.family = AF_UNIX;
    const char *bad = "/tmp/enotdir_file/sock";
    int j = 0; while (bad[j]) { sun.path[j] = bad[j]; j++; }

    long r = sc3(SYS_BIND, sfd, (long)&sun, 110);
    check_val("bind ENOTDIR", r, -ENOTDIR);

    sc1(SYS_CLOSE, sfd);
    sc1(SYS_UNLINK, (long)"/tmp/enotdir_file");
}

/* ── connect01 — EINVAL salen ── */
static void test_connect01_einval_salen(void) {
    puts("\n[ltp/connect01-einval-salen]\n");

    long sfd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("socket", sfd >= 0);
    if (sfd < 0) return;

    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char *)&sa)[i] = 0;
    sa.family = AF_INET;
    sa.addr = 0x0100007F;
    sa.port = 0x5000;

    long r = sc3(SYS_CONNECT, sfd, (long)&sa, 3);
    check_val("connect salen=3 EINVAL", r, -EINVAL);

    sc1(SYS_CLOSE, sfd);
}

/* ── connect01 — EFAULT for bad addr pointer ── */
static void test_connect01_efault(void) {
    puts("\n[ltp/connect01-efault]\n");

    long sfd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("socket", sfd >= 0);
    if (sfd < 0) return;

    /* Kernel pointer guaranteed to fail user_ok */
    long r = sc3(SYS_CONNECT, sfd, (long)-1, 16);
    check_val("connect bad-addr EFAULT", r, -EFAULT);

    sc1(SYS_CLOSE, sfd);
}

/* ── accept03 — EBADF on O_PATH fd ── */
static void test_accept03_opath(void) {
    puts("\n[ltp/accept03-opath]\n");

    long fd = sc3(SYS_OPEN, (long)"/dev/null", O_PATH, 0);
    check("open O_PATH", fd >= 0);
    if (fd < 0) return;

    long r = sc3(SYS_ACCEPT, fd, 0, 0);
    check_val("accept O_PATH EBADF", r, -EBADF);

    sc1(SYS_CLOSE, fd);
}

/* ── AF_UNIX abstract namespace roundtrip ──
 * bind server to "\0cosmo_abstract", connect client, accept, read/write. */
static void test_unix_abstract_bind(void) {
    puts("\n[ltp/unix-abstract-bind]\n");

    long lfd = sc3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
    check("unix listen", lfd >= 0);
    if (lfd < 0) return;

    struct { uint16_t family; char path[108]; } sun;
    for (int i = 0; i < 110; i++) ((char *)&sun)[i] = 0;
    sun.family = AF_UNIX;
    /* Abstract: first byte NUL, name follows */
    sun.path[0] = '\0';
    const char *name = "cosmo_abstract";
    int j = 0; while (name[j]) { sun.path[1 + j] = name[j]; j++; }

    int addrlen = 2 + 1 + j; /* family + NUL + name bytes */
    long r = sc3(SYS_BIND, lfd, (long)&sun, addrlen);
    check_val("bind abstract", r, 0);

    r = sc2(SYS_LISTEN, lfd, 5);
    check_val("listen abstract", r, 0);

    /* Second bind to same abstract name → EADDRINUSE */
    long dup = sc3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
    if (dup >= 0) {
        r = sc3(SYS_BIND, dup, (long)&sun, addrlen);
        check_val("dup abstract bind EADDRINUSE", r, -EADDRINUSE);
        sc1(SYS_CLOSE, dup);
    }

    /* Client in child: connect+write */
    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) { sc1(SYS_CLOSE, lfd); return; }

    if (pid == 0) {
        long cfd = sc3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
        if (cfd >= 0) {
            long cr = sc3(SYS_CONNECT, cfd, (long)&sun, addrlen);
            if (cr == 0) sc3(SYS_WRITE, cfd, (long)"Z", 1);
            sc1(SYS_CLOSE, cfd);
            sc1(SYS_EXIT_GROUP, (cr == 0) ? 0 : 1);
        }
        sc1(SYS_EXIT_GROUP, 2);
        __builtin_unreachable();
    }

    long afd = sc3(SYS_ACCEPT, lfd, 0, 0);
    if (afd >= 0) {
        char buf = 0;
        long n = sc3(SYS_READ, afd, (long)&buf, 1);
        check_val("read byte", n, 1);
        check_val("byte is Z", (long)buf, (long)'Z');
        sc1(SYS_CLOSE, afd);
    } else {
        fail("abstract accept", "accept failed");
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    sc1(SYS_CLOSE, lfd);
}

/* ── Abstract: getsockname returns correct out_len (family + raw bytes,
 *    no trailing NUL) ── */
static void test_unix_abstract_getsockname(void) {
    puts("\n[ltp/unix-abstract-getsockname]\n");

    long sfd = sc3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
    check("unix socket", sfd >= 0);
    if (sfd < 0) return;

    struct { uint16_t family; char path[108]; } sun;
    for (int i = 0; i < 110; i++) ((char *)&sun)[i] = 0;
    sun.family = AF_UNIX;
    sun.path[0] = '\0';
    const char *name = "abs_getsockname";
    int j = 0; while (name[j]) { sun.path[1 + j] = name[j]; j++; }
    int addrlen = 2 + 1 + j;

    long r = sc3(SYS_BIND, sfd, (long)&sun, addrlen);
    check_val("abstract bind", r, 0);
    if (r != 0) { sc1(SYS_CLOSE, sfd); return; }

    struct { uint16_t family; char path[108]; } got;
    for (int i = 0; i < 110; i++) ((char *)&got)[i] = 0;
    int got_len = 110;
    r = sc3(SYS_GETSOCKNAME, sfd, (long)&got, (long)&got_len);
    check_val("getsockname rc", r, 0);
    /* Abstract: out_len = family(2) + 1 + name_len (no NUL) */
    check_val("abstract out_len", (long)got_len, (long)addrlen);
    check_val("got family", (long)got.family, (long)AF_UNIX);
    check_val("leading NUL preserved", (long)got.path[0], 0);

    sc1(SYS_CLOSE, sfd);
}

TEST("ltp/bind01-einval-salen",     test_bind01_einval_salen);
TEST("ltp/bind01-eafnosupport",     test_bind01_eafnosupport);
TEST("ltp/bind01-eaddrnotavail",    test_bind01_eaddrnotavail);
TEST("ltp/bind01-enotdir",          test_bind01_enotdir);
TEST("ltp/connect01-einval-salen",  test_connect01_einval_salen);
TEST("ltp/connect01-efault",        test_connect01_efault);
TEST("ltp/accept03-opath",          test_accept03_opath);
TEST("ltp/unix-abstract-bind",      test_unix_abstract_bind);
TEST("ltp/unix-abstract-getsockname", test_unix_abstract_getsockname);
