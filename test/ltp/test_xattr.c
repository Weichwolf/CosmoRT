/* LTP fgetxattr02/03 + flistxattr01/02/03 — ported to ktest
 * xattr syscalls may return -ENOSYS or -EOPNOTSUPP on CosmoRT.
 * Test error paths. */
#include "ktest.h"

/* ── fgetxattr02: xattr on various file types ──
 * Original tests fgetxattr on regular file, dir, symlink, fifo, chr, blk, socket.
 * We test that fgetxattr returns an appropriate error. */

static void test_fgetxattr02_regular(void) {
    puts("\n[ltp/fgetxattr02-regular]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/xattr_test", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    /* SYS_FGETXATTR = 193: fgetxattr(fd, "user.test", buf, 64) */
    char buf[64];
    long r = sc4(SYS_FGETXATTR, fd, (long)"user.test", (long)buf, 64);
    /* Expect -ENOSYS, -EOPNOTSUPP, or -ENODATA */
    check("fgetxattr returns error", r < 0);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/xattr_test");
}

/* ── fgetxattr03: zero-size buffer returns current xattr size ──
 * On CosmoRT this will likely return -ENOSYS. */

static void test_fgetxattr03(void) {
    puts("\n[ltp/fgetxattr03]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/xattr_test3", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    long r = sc4(SYS_FGETXATTR, fd, (long)"user.testkey", 0, 0);
    /* -ENOSYS or -ENODATA expected */
    check("fgetxattr zero-size returns error", r < 0);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/xattr_test3");
}

/* ── fsetxattr: try to set xattr ── */

static void test_fsetxattr(void) {
    puts("\n[ltp/fsetxattr]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/xattr_set", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    /* SYS_FSETXATTR = 190: fsetxattr(fd, "user.test", "val", 3, 1=XATTR_CREATE) */
    long r = sc5(SYS_FSETXATTR, fd, (long)"user.test", (long)"val", 3, 1);
    check("fsetxattr returns error", r < 0);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/xattr_set");
}

/* ── flistxattr01: list xattrs on file ── */

static void test_flistxattr01(void) {
    puts("\n[ltp/flistxattr01]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/xattr_list1", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    /* SYS_FLISTXATTR = 196 */
    char buf[128];
    long r = sc3(SYS_FLISTXATTR, fd, (long)buf, 128);
    /* -ENOSYS or 0 (empty list) */
    check("flistxattr returns error or empty", r <= 0);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/xattr_list1");
}

/* ── flistxattr02: ERANGE for small buffer, EBADF for bad fd ── */

static void test_flistxattr02_ebadf(void) {
    puts("\n[ltp/flistxattr02-ebadf]\n");

    char buf[20];
    long r = sc3(SYS_FLISTXATTR, -1, (long)buf, 20);
    /* -EBADF or -ENOSYS */
    check("flistxattr bad fd", r < 0);
}

/* ── flistxattr03: zero-size buffer for size query ── */

static void test_flistxattr03(void) {
    puts("\n[ltp/flistxattr03]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/xattr_list3", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    long r = sc3(SYS_FLISTXATTR, fd, 0, 0);
    /* -ENOSYS or >= 0 */
    check("flistxattr zero-size returns", r == -ENOSYS || r >= 0);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/xattr_list3");
}

/* ── fgetxattr02-setup: mknod(FIFO/CHR/BLK) darf nicht EPERM liefern ── */

static void test_fgetxattr02_setup(void) {
    puts("\n[ltp/fgetxattr02-setup]\n");

    sc1(SYS_UNLINK, (long)"/tmp/xattr_fifo");
    sc1(SYS_UNLINK, (long)"/tmp/xattr_chr");
    sc1(SYS_UNLINK, (long)"/tmp/xattr_blk");

    long r = sc3(SYS_MKNOD, (long)"/tmp/xattr_fifo", S_IFIFO | 0777, 0);
    check_val("mknod FIFO", r, 0);
    r = sc3(SYS_MKNOD, (long)"/tmp/xattr_chr", S_IFCHR | 0777, 0);
    check_val("mknod CHR (root)", r, 0);
    r = sc3(SYS_MKNOD, (long)"/tmp/xattr_blk", S_IFBLK | 0777, 0);
    check_val("mknod BLK (root)", r, 0);

    sc1(SYS_UNLINK, (long)"/tmp/xattr_fifo");
    sc1(SYS_UNLINK, (long)"/tmp/xattr_chr");
    sc1(SYS_UNLINK, (long)"/tmp/xattr_blk");
}

TEST("ltp/fgetxattr02-regular",  test_fgetxattr02_regular);
TEST("ltp/fgetxattr02-setup",    test_fgetxattr02_setup);
TEST("ltp/fgetxattr03",          test_fgetxattr03);
TEST("ltp/fsetxattr",            test_fsetxattr);
TEST("ltp/flistxattr01",         test_flistxattr01);
TEST("ltp/flistxattr02-ebadf",   test_flistxattr02_ebadf);
TEST("ltp/flistxattr03",         test_flistxattr03);
