/* LTP faccessat01-02 + faccessat201-202 — ported to ktest */
#include "ktest.h"

#define AT_EACCESS      0x200

/* ── faccessat01: basic functionality ── */

static void test_faccessat01(void) {
    puts("\n[ltp/faccessat01]\n");

    sc2(SYS_MKDIR, (long)"/tmp/faccessat01", 0700);
    long fd = sc3(SYS_OPEN, (long)"/tmp/faccessat01/testfile",
                  O_RDWR | O_CREAT, 0600);
    check("create file", fd >= 0);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    /* dir_fd + relative path */
    long dfd = sc3(SYS_OPEN, (long)"/tmp/faccessat01", O_RDONLY | O_DIRECTORY, 0);
    check("open dir", dfd >= 0);
    if (dfd < 0) return;

    long r = sc4(SYS_FACCESSAT, dfd, (long)"testfile", R_OK, 0);
    check_val("dirfd + relative", r, 0);

    /* AT_FDCWD + relative path */
    long tmpfd = sc3(SYS_OPEN, (long)"/tmp/faccessat01", O_RDONLY | O_DIRECTORY, 0);
    sc1(SYS_FCHDIR, tmpfd);
    sc1(SYS_CLOSE, tmpfd);

    r = sc4(SYS_FACCESSAT, AT_FDCWD, (long)"testfile", R_OK, 0);
    check_val("AT_FDCWD + relative", r, 0);

    /* Restore cwd */
    tmpfd = sc3(SYS_OPEN, (long)"/tmp", O_RDONLY | O_DIRECTORY, 0);
    if (tmpfd >= 0) { sc1(SYS_FCHDIR, tmpfd); sc1(SYS_CLOSE, tmpfd); }

    /* bad fd + absolute path */
    r = sc4(SYS_FACCESSAT, -1, (long)"/tmp/faccessat01/testfile", R_OK, 0);
    check_val("bad fd + abspath", r, 0);

    sc1(SYS_CLOSE, dfd);
    sc1(SYS_UNLINK, (long)"/tmp/faccessat01/testfile");
    sc1(SYS_RMDIR, (long)"/tmp/faccessat01");
}

/* ── faccessat02: error conditions ── */

static void test_faccessat02(void) {
    puts("\n[ltp/faccessat02]\n");

    sc2(SYS_MKDIR, (long)"/tmp/faccessat02", 0700);
    long fd = sc3(SYS_OPEN, (long)"/tmp/faccessat02/testfile",
                  O_RDWR | O_CREAT, 0600);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    /* ENOTDIR: file fd + relative path */
    long ffd = sc3(SYS_OPEN, (long)"/tmp/faccessat02/testfile", O_RDONLY, 0);
    check("open file", ffd >= 0);
    if (ffd >= 0) {
        long r = sc4(SYS_FACCESSAT, ffd, (long)"testfile", R_OK, 0);
        check_val("file fd ENOTDIR", r, -ENOTDIR);
        sc1(SYS_CLOSE, ffd);
    }

    /* EBADF: invalid fd + relative path */
    long r = sc4(SYS_FACCESSAT, -1, (long)"testfile", R_OK, 0);
    check_val("bad fd EBADF", r, -EBADF);

    sc1(SYS_UNLINK, (long)"/tmp/faccessat02/testfile");
    sc1(SYS_RMDIR, (long)"/tmp/faccessat02");
}

/* ── faccessat201: basic faccessat2 functionality ── */

static void test_faccessat201(void) {
    puts("\n[ltp/faccessat201]\n");

    sc2(SYS_MKDIR, (long)"/tmp/faccessat201", 0777);
    long fd = sc3(SYS_OPEN, (long)"/tmp/faccessat201/testfile",
                  O_RDWR | O_CREAT, 0444);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    long dfd = sc3(SYS_OPEN, (long)"/tmp/faccessat201", O_RDONLY | O_DIRECTORY, 0);
    check("open dir", dfd >= 0);
    if (dfd < 0) return;

    /* flags=0 */
    long r = sc4(SYS_FACCESSAT2, dfd, (long)"testfile", R_OK, 0);
    if (r == -ENOSYS) {
        puts("  SKIP  faccessat2 not implemented\n");
        sc1(SYS_CLOSE, dfd);
        sc1(SYS_UNLINK, (long)"/tmp/faccessat201/testfile");
        sc1(SYS_RMDIR, (long)"/tmp/faccessat201");
        return;
    }
    check_val("faccessat2 dirfd", r, 0);

    /* AT_EACCESS */
    r = sc4(SYS_FACCESSAT2, dfd, (long)"testfile", R_OK, AT_EACCESS);
    check_val("faccessat2 AT_EACCESS", r, 0);

    /* absolute path with bad fd */
    r = sc4(SYS_FACCESSAT2, -1, (long)"/tmp/faccessat201/testfile", R_OK, 0);
    check_val("faccessat2 abspath", r, 0);

    sc1(SYS_CLOSE, dfd);
    sc1(SYS_UNLINK, (long)"/tmp/faccessat201/testfile");
    sc1(SYS_RMDIR, (long)"/tmp/faccessat201");
}

/* ── faccessat202: faccessat2 error conditions ── */

static void test_faccessat202(void) {
    puts("\n[ltp/faccessat202]\n");

    sc2(SYS_MKDIR, (long)"/tmp/faccessat202", 0666);
    long fd = sc3(SYS_OPEN, (long)"/tmp/faccessat202/testfile",
                  O_RDWR | O_CREAT, 0444);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    /* EINVAL: invalid flags */
    long r = sc4(SYS_FACCESSAT2, AT_FDCWD,
                 (long)"/tmp/faccessat202/testfile", R_OK, -1);
    if (r == -ENOSYS) {
        puts("  SKIP  faccessat2 not implemented\n");
        sc1(SYS_UNLINK, (long)"/tmp/faccessat202/testfile");
        sc1(SYS_RMDIR, (long)"/tmp/faccessat202");
        return;
    }
    check_val("invalid flags EINVAL", r, -EINVAL);

    /* EINVAL: invalid mode */
    r = sc4(SYS_FACCESSAT2, AT_FDCWD,
            (long)"/tmp/faccessat202/testfile", -1, 0);
    check_val("invalid mode EINVAL", r, -EINVAL);

    /* EBADF: bad fd + relative path */
    r = sc4(SYS_FACCESSAT2, -1, (long)"testfile", R_OK, 0);
    check_val("bad fd EBADF", r, -EBADF);

    /* ENOTDIR: file fd + relative path */
    long ffd = sc3(SYS_OPEN, (long)"/tmp/faccessat202/testfile", O_RDONLY, 0);
    if (ffd >= 0) {
        r = sc4(SYS_FACCESSAT2, ffd, (long)"testfile", R_OK, 0);
        check_val("file fd ENOTDIR", r, -ENOTDIR);
        sc1(SYS_CLOSE, ffd);
    }

    sc1(SYS_UNLINK, (long)"/tmp/faccessat202/testfile");
    sc1(SYS_RMDIR, (long)"/tmp/faccessat202");
}

TEST("ltp/faccessat01",  test_faccessat01);
TEST("ltp/faccessat02",  test_faccessat02);
TEST("ltp/faccessat201", test_faccessat201);
TEST("ltp/faccessat202", test_faccessat202);
