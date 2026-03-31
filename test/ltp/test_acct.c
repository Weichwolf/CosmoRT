/* LTP acct01 — ported to ktest */
#include "ktest.h"

/* ── acct01: error paths ── */

static void test_acct_eisdir(void) {
    puts("\n[ltp/acct01-eisdir]\n");

    long r = sc1(SYS_ACCT, (long)".");
    check_val("acct(\".\") EISDIR", r, -EISDIR);
}

static void test_acct_enoent(void) {
    puts("\n[ltp/acct01-enoent]\n");

    long r = sc1(SYS_ACCT, (long)"/tmp/does/not/exist");
    check_val("acct nonexistent ENOENT", r, -ENOENT);
}

static void test_acct_enotdir(void) {
    puts("\n[ltp/acct01-enotdir]\n");

    /* create a regular file, then use it as path component */
    long fd = sc3(SYS_OPEN, (long)"/tmp/acct_tmpfile", O_RDWR | O_CREAT, 0777);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    long r = sc1(SYS_ACCT, (long)"/tmp/acct_tmpfile/foo");
    check_val("acct ENOTDIR", r, -ENOTDIR);

    sc1(SYS_UNLINK, (long)"/tmp/acct_tmpfile");
}

static void test_acct_efault(void) {
    puts("\n[ltp/acct01-efault]\n");

    /* map a PROT_NONE page to get a guaranteed bad address */
    long bad = sc6(SYS_MMAP, 0, 4096, 0 /* PROT_NONE */, MAP_PRIV_ANON, -1, 0);
    if (bad <= 0) {
        fail("mmap for bad addr", "mmap failed");
        return;
    }

    long r = sc1(SYS_ACCT, bad);
    check_val("acct bad addr EFAULT", r, -EFAULT);

    sc2(SYS_MUNMAP, bad, 4096);
}

static void test_acct_null(void) {
    puts("\n[ltp/acct01-null]\n");

    /* acct(NULL) disables accounting — should succeed or ENOSYS */
    long r = sc1(SYS_ACCT, 0);
    check("acct(NULL) succeeds or ENOSYS", r == 0 || r == -ENOSYS);
}

TEST("ltp/acct01-eisdir",  test_acct_eisdir);
TEST("ltp/acct01-enoent",  test_acct_enoent);
TEST("ltp/acct01-enotdir", test_acct_enotdir);
TEST("ltp/acct01-efault",  test_acct_efault);
TEST("ltp/acct01-null",    test_acct_null);
