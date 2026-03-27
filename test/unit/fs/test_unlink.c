#include "ktest.h"

/* ── unlink frees data blocks ────────────────── */

#define SYS_STATFS  137

struct k_statfs {
    long f_type, f_bsize, f_blocks, f_bfree, f_bavail;
    long f_files, f_ffree;
    long f_fsid[2];
    long f_namelen, f_frsize;
    long f_flags;
    long f_spare[4];
};

static void test_unlink_basic(void) {
    puts("\n[unlink-basic]\n");

    /* Create file, write data, unlink, verify gone */
    long fd = sc3(SYS_OPEN, (long)"/tmp/ul_test", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    check("create /tmp/ul_test", fd >= 0);
    if (fd >= 0) {
        const char *data = "unlink test data";
        sc3(SYS_WRITE, fd, (long)data, 16);
        sc1(SYS_CLOSE, fd);
    }

    long r = sc1(SYS_UNLINK, (long)"/tmp/ul_test");
    check_val("unlink returns 0", r, 0);

    fd = sc3(SYS_OPEN, (long)"/tmp/ul_test", O_RDONLY, 0);
    check("open after unlink → ENOENT", fd == -ENOENT);
    if (fd >= 0) sc1(SYS_CLOSE, fd);
}

TEST("unlink-basic", test_unlink_basic);

/* ── unlink 100 files: no disk-space leak ────── */

static void test_unlink_space(void) {
    puts("\n[unlink-space]\n");

    /* Snapshot free blocks before */
    struct k_statfs st_before, st_after;
    long r = sc2(SYS_STATFS, (long)"/tmp", (long)&st_before);
    check_val("statfs /tmp before", r, 0);

    /* Create and delete 100 files, each 4KB */
    char path[32] = "/tmp/ulk_";
    char buf[4096];
    for (int i = 0; i < 4096; i++) buf[i] = 'A';

    for (int i = 0; i < 100; i++) {
        /* Build path: /tmp/ulk_NN */
        path[9] = '0' + (char)(i / 10);
        path[10] = '0' + (char)(i % 10);
        path[11] = 0;

        long fd = sc3(SYS_OPEN, (long)path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            sc3(SYS_WRITE, fd, (long)buf, 4096);
            sc1(SYS_CLOSE, fd);
        }
        sc1(SYS_UNLINK, (long)path);
    }

    /* Snapshot free blocks after */
    r = sc2(SYS_STATFS, (long)"/tmp", (long)&st_after);
    check_val("statfs /tmp after", r, 0);

    /* /tmp is ramfs — free blocks tracked differently.
     * For CosmoFS paths, we'd compare f_bfree.
     * For ramfs, just verify no crash and basic sanity. */
    check("no space leak (bfree after >= before)",
          st_after.f_bfree >= st_before.f_bfree);
}

TEST("unlink-space", test_unlink_space);

/* ── xattr returns ENODATA, not ENOSYS ──────── */

#define SYS_GETXATTR  191

static void test_xattr_enodata(void) {
    puts("\n[xattr-enodata]\n");

    long r = sc4(SYS_GETXATTR, (long)"/tmp", (long)"user.test", (long)0, 0);
    check_val("getxattr → -ENODATA", r, -ENODATA);
}

TEST("xattr-enodata", test_xattr_enodata);
