#include "ktest.h"

/* ── Test: 512+ FDs via dup ───────────────────── */
static void test_fd_scale_512(void) {
    puts("\n[FD_SCALE_512]\n");

    int count = 0;
    for (int i = 0; i < 600; i++) {
        long fd = sc1(SYS_DUP, 0);
        if (fd < 0) break;
        count++;
    }

    check_ge("512+ FDs opened", (long)count, 512);

    for (int i = 3; i < count + 3; i++)
        sc1(SYS_CLOSE, i);
}

/* ── Test: close + re-open returns lowest free FD (POSIX) ── */
static void test_fd_lowest_free(void) {
    puts("\n[FD_LOWEST_FREE]\n");

    int fds[8];
    for (int i = 0; i < 8; i++)
        fds[i] = (int)sc1(SYS_DUP, 0);

    int target = fds[2];
    sc1(SYS_CLOSE, target);

    long newfd = sc1(SYS_DUP, 0);
    check_val("re-alloc returns closed fd", newfd, (long)target);

    sc1(SYS_CLOSE, newfd);
    for (int i = 0; i < 8; i++) {
        if (fds[i] != target)
            sc1(SYS_CLOSE, fds[i]);
    }
}

/* ── Test: exhaust RLIMIT_NOFILE → EMFILE ─────────────── */
static void test_fd_emfile(void) {
    puts("\n[FD_EMFILE]\n");

    for (int i = 3; i < 2048; i++)
        sc1(SYS_CLOSE, i);

    int count = 0;
    long max_fd = 0;
    for (;;) {
        long fd = sc1(SYS_DUP, 0);
        if (fd < 0) {
            check_val("dup fails with EMFILE", fd, -EMFILE);
            break;
        }
        if (fd > max_fd) max_fd = fd;
        count++;
        if (count > 1100) {
            check("reached EMFILE before overflow", 0);
            break;
        }
    }

    check("all fds < ulimit (1024)", max_fd < 1024);
    check_ge("opened near ulimit fds", (long)count, 1000);

    for (int i = 3; i < count + 3; i++)
        sc1(SYS_CLOSE, i);
}

/* ── Test: setrlimit(RLIMIT_NOFILE, 2048) opens 2000+ FDs ───── */
#define TEST_RLIMIT_NOFILE 7
struct t_rlimit { unsigned long cur, max; };

static void test_fd_expand_via_rlimit(void) {
    puts("\n[FD_EXPAND_RLIMIT]\n");

    for (int i = 3; i < 2048; i++) sc1(SYS_CLOSE, i);

    struct t_rlimit r = { 4096, 65536 };
    long sr = sc4(SYS_PRLIMIT64, 0, TEST_RLIMIT_NOFILE, (long)&r, 0);
    check_val("prlimit64 set NOFILE=4096", sr, 0);

    int count = 0;
    for (int i = 0; i < 3500; i++) {
        long fd = sc1(SYS_DUP, 0);
        if (fd < 0) break;
        count++;
    }
    check_ge("opened >2000 FDs after rlimit raise", (long)count, 2000);

    for (int i = 3; i < count + 3; i++) sc1(SYS_CLOSE, i);

    struct t_rlimit back = { 1024, 65536 };
    sc4(SYS_PRLIMIT64, 0, TEST_RLIMIT_NOFILE, (long)&back, 0);
}

TEST("fd_scale_512", test_fd_scale_512);
TEST("fd_lowest_free", test_fd_lowest_free);
TEST("fd_emfile", test_fd_emfile);
TEST("fd_expand_rlimit", test_fd_expand_via_rlimit);
