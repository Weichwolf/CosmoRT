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

/* ── Test: open(2) scales past old 512-entry file_pool cap ── */
static void test_file_slab_grows(void) {
    puts("\n[FILE_SLAB_GROWS]\n");

    sc3(SYS_OPEN,   (long)"/tmp/fslab_probe", O_CREAT | O_WRONLY | O_TRUNC, 0644);

    int fds[700];
    int count = 0;
    for (int i = 0; i < 700; i++) {
        long fd = sc3(SYS_OPEN, (long)"/tmp/fslab_probe", O_RDONLY, 0);
        if (fd < 0) break;
        fds[count++] = (int)fd;
    }
    check_ge("open(2) >512 times (dynamic file_slab)", (long)count, 600);

    for (int i = 0; i < count; i++) sc1(SYS_CLOSE, fds[i]);
}

/* ── Phase 13.1: page-list FD table — past old buddy-cap of 65536 ── */

/* prlimit64 reports rlim_max == FD_CEILING. Pre-Phase-13.1 was 65536;
 * Linux default (and ours now) is 1<<20. */
static void test_fd_ceiling_is_one_million(void) {
    puts("\n[FD_CEILING_1M]\n");
    struct t_rlimit cur = { 0, 0 };
    long r = sc4(SYS_PRLIMIT64, 0, TEST_RLIMIT_NOFILE, 0, (long)&cur);
    check_val("prlimit64 read ok", r, 0);
    check_val("rlim_max == 1<<20", (long)cur.max, 1L << 20);
}

/* Open enough FDs to span more than one leaf page (170 fds per page).
 * Touches: lazy fd_leaf_alloc, fd_install_at across leaf boundary, and
 * fd_close + reuse inside the same leaf. */
static void test_fd_cross_leaf_pages(void) {
    puts("\n[FD_CROSS_LEAF]\n");
    /* Raise nofile past the natural 1024 default so we can comfortably
     * span 5+ leaf pages (5*170 = 850; we ask for 1500 fds). */
    struct t_rlimit r = { 4096, 1L << 20 };
    sc4(SYS_PRLIMIT64, 0, TEST_RLIMIT_NOFILE, (long)&r, 0);

    int allocated = 0;
    int last_fd = 2;
    for (int i = 0; i < 1500; i++) {
        long fd = sc1(SYS_DUP, 0);
        if (fd < 0) break;
        if (fd > last_fd) last_fd = (int)fd;
        allocated++;
    }
    check_ge("dup >850 fds spans 5+ leaves", (long)allocated, 850);
    check_ge("highest fd >= 853 (3 stdio + 850)", (long)last_fd, 853);

    /* Close every 7th fd, then re-allocate — cleanup path must clear the
     * bitmap word on the right leaf, not crash on a NULL leaf pointer. */
    int reclaimed = 0;
    for (int fd = 10; fd <= last_fd; fd += 7) {
        if (sc1(SYS_CLOSE, fd) == 0) reclaimed++;
    }
    int reopened = 0;
    for (int i = 0; i < reclaimed; i++) {
        long fd = sc1(SYS_DUP, 0);
        if (fd < 0) break;
        reopened++;
    }
    check_val("re-dup recovers all closed fds", (long)reopened, (long)reclaimed);

    for (int i = 3; i <= last_fd + reopened; i++) sc1(SYS_CLOSE, i);

    struct t_rlimit back = { 1024, 1L << 20 };
    sc4(SYS_PRLIMIT64, 0, TEST_RLIMIT_NOFILE, (long)&back, 0);
}

/* Open a fd at a deliberately-high index (newfd via dup3) so the dir
 * grows multiple times and a far-away leaf gets touched without
 * intermediate leaves wasting pages. nofile must be >= newfd; we raise
 * it past the old 65536 ceiling so the second dup3 can target 70000. */
static void test_fd_high_index_install(void) {
    puts("\n[FD_HIGH_INDEX]\n");
    struct t_rlimit r = { 100000, 1L << 20 };
    sc4(SYS_PRLIMIT64, 0, TEST_RLIMIT_NOFILE, (long)&r, 0);

    /* dup3(0, 12345, 0) — installs into fd 12345, growing dir + bitmap
     * but only one leaf page. */
    long high = sc3(292 /* SYS_DUP3 */, 0, 12345, 0);
    check_val("dup3 to fd=12345 ok", high, 12345);
    /* And a really high index that crosses the 65536 old ceiling. */
    long higher = sc3(292 /* SYS_DUP3 */, 0, 70000, 0);
    check_val("dup3 to fd=70000 ok (past old 65536 cap)", higher, 70000);

    sc1(SYS_CLOSE, 12345);
    sc1(SYS_CLOSE, 70000);

    struct t_rlimit back = { 1024, 1L << 20 };
    sc4(SYS_PRLIMIT64, 0, TEST_RLIMIT_NOFILE, (long)&back, 0);
}

TEST("fd_scale_512", test_fd_scale_512);
TEST("fd_lowest_free", test_fd_lowest_free);
TEST("fd_emfile", test_fd_emfile);
TEST("fd_expand_rlimit", test_fd_expand_via_rlimit);
TEST("file_slab_grows", test_file_slab_grows);
TEST("fd_ceiling_1m",      test_fd_ceiling_is_one_million);
TEST("fd_cross_leaf",      test_fd_cross_leaf_pages);
TEST("fd_high_index",      test_fd_high_index_install);
