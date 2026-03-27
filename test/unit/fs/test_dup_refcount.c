#include "ktest.h"

static void test_dup_refcount(void) {
    puts("\n[dup refcount]\n");

    /* ── eventfd: dup3 then close original ──────── */
    long efd = sc2(SYS_EVENTFD2, 0, 0);
    check("eventfd2 >= 0", efd >= 0);
    if (efd >= 0) {
        int newfd = 50;
        long r = sc3(SYS_DUP3, efd, newfd, 0);
        check_val("dup3(eventfd) returns newfd", r, newfd);
        sc1(SYS_CLOSE, efd); /* close original */

        /* Write to dup'd fd — must not crash */
        uint64_t val = 1;
        long w = sc3(SYS_WRITE, newfd, (long)&val, (long)sizeof(val));
        check_val("write(dup'd eventfd) = 8", w, 8);

        /* Read back */
        uint64_t rval = 0;
        long rd = sc3(SYS_READ, newfd, (long)&rval, (long)sizeof(rval));
        check_val("read(dup'd eventfd) = 8", rd, 8);
        check_val("eventfd counter = 1", (long)rval, 1);

        sc1(SYS_CLOSE, newfd);
    }

    /* ── pipe: dup3 read end, close original, read from dup ── */
    int pfd[2] = { -1, -1 };
    long r = sc2(SYS_PIPE2, (long)pfd, 0);
    check_val("pipe2 returns 0", r, 0);
    if (r == 0) {
        int dup_read = 51;
        r = sc3(SYS_DUP3, pfd[0], dup_read, 0);
        check_val("dup3(pipe read) returns newfd", r, dup_read);
        sc1(SYS_CLOSE, pfd[0]); /* close original read end */

        /* Write to pipe write end */
        const char *msg = "dup";
        long w = sc3(SYS_WRITE, pfd[1], (long)msg, 3);
        check_val("write(pipe) = 3", w, 3);

        /* Read from dup'd read end */
        char buf[8] = {0};
        long rd = sc3(SYS_READ, dup_read, (long)buf, 8);
        check_val("read(dup'd pipe) = 3", rd, 3);
        check("pipe dup data 'd'", buf[0] == 'd');

        sc1(SYS_CLOSE, pfd[1]);
        sc1(SYS_CLOSE, dup_read);
    }

    /* ── unix socket: dup3, close original, write to dup ── */
    int sv[2] = { -1, -1 };
    r = sc4(SYS_SOCKETPAIR, 1 /* AF_UNIX */, 1 /* SOCK_STREAM */, 0, (long)sv);
    check_val("socketpair returns 0", r, 0);
    if (r == 0) {
        int dup_sv = 52;
        r = sc3(SYS_DUP3, sv[0], dup_sv, 0);
        check_val("dup3(unix sock) returns newfd", r, dup_sv);
        sc1(SYS_CLOSE, sv[0]); /* close original */

        /* Write to dup'd socket */
        const char *msg = "usock";
        long w = sc3(SYS_WRITE, dup_sv, (long)msg, 5);
        check_val("write(dup'd unix sock) = 5", w, 5);

        /* Read from peer */
        char buf[8] = {0};
        long rd = sc3(SYS_READ, sv[1], (long)buf, 8);
        check_val("read(peer) = 5", rd, 5);
        check("usock dup data 'u'", buf[0] == 'u');

        sc1(SYS_CLOSE, dup_sv);
        sc1(SYS_CLOSE, sv[1]);
    }
}

TEST("dup_refcount", test_dup_refcount);
