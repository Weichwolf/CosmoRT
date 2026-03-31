/* LTP chdir02/chdir04 — ported to ktest */
#include "ktest.h"

/* ── chdir02: all slash-only paths of length 1..N resolve to root ── */

static void test_chdir02(void) {
    puts("\n[ltp/chdir02]\n");

    char path[64];
    int fail_count = 0;

    /* Test a few representative lengths of "/" paths */
    int lens[] = {1, 2, 3, 10, 32, 63};
    for (int t = 0; t < 6; t++) {
        int len = lens[t];
        for (int i = 0; i < len; i++) path[i] = '/';
        path[len] = 0;

        long r = sc1(SYS_CHDIR, (long)path);
        if (r != 0) { fail_count++; continue; }

        /* Verify we are at root via getcwd */
        char buf[256];
        r = sc2(SYS_GETCWD, (long)buf, 256);
        if (r < 0 || buf[0] != '/' || buf[1] != 0)
            fail_count++;
    }

    check_val("slash paths all resolve to /", (long)fail_count, 0);
}

/* ── chdir04: error cases ── */

static void test_chdir04_enametoolong(void) {
    puts("\n[ltp/chdir04-enametoolong]\n");

    /* Path component longer than NAME_MAX (typically 255) */
    char long_dir[300];
    for (int i = 0; i < 299; i++) long_dir[i] = 'a';
    long_dir[299] = 0;

    long r = sc1(SYS_CHDIR, (long)long_dir);
    check_val("chdir ENAMETOOLONG", r, -ENAMETOOLONG);
}

static void test_chdir04_enoent(void) {
    puts("\n[ltp/chdir04-enoent]\n");

    long r = sc1(SYS_CHDIR, (long)"/tmp/chdir04_nonexist");
    check_val("chdir ENOENT", r, -ENOENT);
}

static void test_chdir04_enotdir(void) {
    puts("\n[ltp/chdir04-enotdir]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/chdir04_file", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    long r = sc1(SYS_CHDIR, (long)"/tmp/chdir04_file");
    check_val("chdir to file ENOTDIR", r, -ENOTDIR);

    sc1(SYS_UNLINK, (long)"/tmp/chdir04_file");
}

/* ── chdir basic: chdir + getcwd round-trip ── */

static void test_chdir_basic(void) {
    puts("\n[ltp/chdir-basic]\n");

    sc2(SYS_MKDIR, (long)"/tmp/chdir_basic_dir", 0755);

    long r = sc1(SYS_CHDIR, (long)"/tmp/chdir_basic_dir");
    check_val("chdir", r, 0);

    char buf[256];
    r = sc2(SYS_GETCWD, (long)buf, 256);
    check("getcwd", r > 0);

    /* Verify the path ends with our directory name */
    int found = 0;
    for (int i = 0; buf[i]; i++) {
        if (buf[i] == 'c' && buf[i+1] == 'h' && buf[i+2] == 'd' &&
            buf[i+3] == 'i' && buf[i+4] == 'r' && buf[i+5] == '_' &&
            buf[i+6] == 'b')
            found = 1;
    }
    check("cwd contains dir name", found);

    /* Restore */
    sc1(SYS_CHDIR, (long)"/");
    sc1(SYS_RMDIR, (long)"/tmp/chdir_basic_dir");
}

TEST("ltp/chdir02",              test_chdir02);
TEST("ltp/chdir04-enametoolong", test_chdir04_enametoolong);
TEST("ltp/chdir04-enoent",       test_chdir04_enoent);
TEST("ltp/chdir04-enotdir",      test_chdir04_enotdir);
TEST("ltp/chdir-basic",          test_chdir_basic);
