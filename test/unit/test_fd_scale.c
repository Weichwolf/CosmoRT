#include "ktest.h"

/* ── Test: 512+ FDs via dup ───────────────────── */
static void test_fd_scale_512(void) {
    puts("\n[FD_SCALE_512]\n");

    int count = 0;
    /* dup stdin (fd 0) repeatedly — no kernel object limit */
    for (int i = 0; i < 600; i++) {
        long fd = sc1(SYS_DUP, 0);
        if (fd < 0) break;
        count++;
    }

    check_ge("512+ FDs opened", (long)count, 512);

    /* Cleanup: close fds 3..count+2 */
    for (int i = 3; i < count + 3; i++)
        sc1(SYS_CLOSE, i);
}

/* ── Test: close + re-open returns lowest free FD (POSIX) ── */
static void test_fd_lowest_free(void) {
    puts("\n[FD_LOWEST_FREE]\n");

    /* Open 8 FDs via dup (fds 3..10) */
    int fds[8];
    for (int i = 0; i < 8; i++)
        fds[i] = (int)sc1(SYS_DUP, 0);

    /* Close fd in the middle (fds[2] = fd 5) */
    int target = fds[2];
    sc1(SYS_CLOSE, target);

    /* Next alloc must return exactly that fd (lowest free) */
    long newfd = sc1(SYS_DUP, 0);
    check_val("re-alloc returns closed fd", newfd, (long)target);

    /* Cleanup */
    sc1(SYS_CLOSE, newfd);
    for (int i = 0; i < 8; i++) {
        if (fds[i] != target)
            sc1(SYS_CLOSE, fds[i]);
    }
}

/* ── Test: exhaust FD_MAX → EMFILE ─────────────── */
static void test_fd_emfile(void) {
    puts("\n[FD_EMFILE]\n");

    int count = 0;
    long last_fd = -1;
    /* Fill all FD slots via dup */
    for (;;) {
        long fd = sc1(SYS_DUP, 0);
        if (fd < 0) {
            check_val("dup fails with EMFILE", fd, -EMFILE);
            break;
        }
        last_fd = fd;
        count++;
        if (count > 1100) {
            puts("  last_fd="); put_int(last_fd); puts(" count="); put_int(count); puts("\n");
            check("reached EMFILE before overflow", 0);
            break;
        }
    }

    /* Started with 3 FDs (0-2), FD_MAX=1024: should open 1021 */
    check_ge("opened near FD_MAX fds", (long)count, 1000);

    /* Cleanup: close fds 3..count+2 */
    for (int i = 3; i < count + 3; i++)
        sc1(SYS_CLOSE, i);
}

TEST("fd_scale_512", test_fd_scale_512);
TEST("fd_lowest_free", test_fd_lowest_free);
TEST("fd_emfile", test_fd_emfile);
