/* test: utimensat bugs — ENOTDIR wrong errno + 64-bit timestamp truncation */
#include "ktest.h"

#define UTIME_OMIT ((1L << 30) - 2L)

/*
 * utimensat on path through non-directory returns ENOENT instead of ENOTDIR.
 * Linux: /dev/null/invalid → ENOTDIR (null is not a dir).
 * CosmoRT: returns ENOENT (wrong).
 */
static void test_utime_enotdir(void) {
    puts("\n[fs/utime-enotdir]\n");

    struct k_timespec ts[2];
    ts[0].tv_sec = 0; ts[0].tv_nsec = UTIME_OMIT;
    ts[1].tv_sec = 0; ts[1].tv_nsec = UTIME_OMIT;

    long r = sc4(SYS_UTIMENSAT, AT_FDCWD,
                 (long)"/dev/null/invalid", (long)ts, 0);
    check_val("utime ENOTDIR", r, -ENOTDIR);
}

/*
 * utimensat with tv_sec = 1LL<<32 gets truncated to 0.
 * Proves 32-bit timestamp truncation bug.
 */
static void test_utime_64bit(void) {
    puts("\n[fs/utime-64bit]\n");

    /* Create temp file */
    long fd = sc3(SYS_OPEN, (long)"/tmp/utime64", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    check("create file", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    /* Set atime to 1LL<<32 */
    long big_ts = 1LL << 32;
    struct k_timespec ts[2];
    ts[0].tv_sec = big_ts;  ts[0].tv_nsec = 0; /* atime */
    ts[1].tv_sec = big_ts;  ts[1].tv_nsec = 0; /* mtime */

    long r = sc4(SYS_UTIMENSAT, AT_FDCWD,
                 (long)"/tmp/utime64", (long)ts, 0);
    check_val("utimensat big ts", r, 0);
    if (r != 0) return;

    /* Read back via fstatat */
    struct k_stat st;
    r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/utime64", (long)&st, 0);
    check_val("fstatat", r, 0);
    if (r != 0) return;

    check_val("atime 64-bit", st.st_atime_sec, big_ts);
    check_val("mtime 64-bit", st.st_mtime_sec, big_ts);

    /* Cleanup */
    sc1(SYS_UNLINK, (long)"/tmp/utime64");
}

TEST("fs/utime-enotdir", test_utime_enotdir);
TEST("fs/utime-64bit", test_utime_64bit);
