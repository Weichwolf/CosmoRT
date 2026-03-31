/* LTP fchdir01-03 — ported to ktest */
#include "ktest.h"

/* ── fchdir01: basic fchdir into a directory ── */

static void test_fchdir01(void) {
    puts("\n[ltp/fchdir01]\n");

    sc2(SYS_MKDIR, (long)"/tmp/fchdir01_dir", 0700);

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchdir01_dir", O_RDONLY | O_DIRECTORY, 0);
    check("open dir", fd >= 0);
    if (fd < 0) return;

    long r = sc1(SYS_FCHDIR, fd);
    check_val("fchdir", r, 0);

    /* Verify cwd changed */
    char buf[256];
    r = sc2(SYS_GETCWD, (long)buf, 256);
    check("getcwd ok", r > 0);

    /* Check path contains "fchdir01_dir" */
    int found = 0;
    for (int i = 0; buf[i] && i < 240; i++) {
        if (buf[i] == 'f' && buf[i+1] == 'c' && buf[i+2] == 'h' &&
            buf[i+3] == 'd' && buf[i+4] == 'i' && buf[i+5] == 'r' &&
            buf[i+6] == '0' && buf[i+7] == '1')
            found = 1;
    }
    check("cwd is fchdir01_dir", found);

    /* Restore to /tmp */
    long tmpfd = sc3(SYS_OPEN, (long)"/tmp", O_RDONLY | O_DIRECTORY, 0);
    if (tmpfd >= 0) { sc1(SYS_FCHDIR, tmpfd); sc1(SYS_CLOSE, tmpfd); }

    sc1(SYS_CLOSE, fd);
    sc1(SYS_RMDIR, (long)"/tmp/fchdir01_dir");
}

/* ── fchdir02: EBADF for bad fd ── */

static void test_fchdir02(void) {
    puts("\n[ltp/fchdir02]\n");

    long r = sc1(SYS_FCHDIR, -5);
    check_val("fchdir(-5) EBADF", r, -EBADF);

    r = sc1(SYS_FCHDIR, 9999);
    check_val("fchdir(9999) EBADF", r, -EBADF);
}

/* ── fchdir03: ENOTDIR for non-directory fd ── */

static void test_fchdir03(void) {
    puts("\n[ltp/fchdir03]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchdir03_file", O_RDWR | O_CREAT, 0666);
    check("open file", fd >= 0);
    if (fd < 0) return;

    long r = sc1(SYS_FCHDIR, fd);
    check_val("fchdir(file) ENOTDIR", r, -ENOTDIR);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fchdir03_file");
}

TEST("ltp/fchdir01", test_fchdir01);
TEST("ltp/fchdir02", test_fchdir02);
TEST("ltp/fchdir03", test_fchdir03);
