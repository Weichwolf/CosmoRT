/* LTP chmod01/03/05/06/07/08 — ported to ktest */
#include "ktest.h"

/* ── chmod01: chmod succeeds for various mode bits on file and dir ── */

static void test_chmod01_file(void) {
    puts("\n[ltp/chmod01-file]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/chmod01_file", O_RDWR | O_CREAT, 0644);
    check("create file", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    int modes[] = {0, 07, 070, 0700, 0777, 02777, 04777, 06777};
    struct k_stat st;

    for (int i = 0; i < 8; i++) {
        long r = sc2(SYS_CHMOD, (long)"/tmp/chmod01_file", modes[i]);
        check("chmod file", r == 0);
        if (r != 0) continue;

        r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/chmod01_file", (long)&st, 0);
        check("stat file", r == 0);
        if (r == 0)
            check_val("mode bits", (long)(st.st_mode & 07777), (long)modes[i]);
    }

    sc1(SYS_UNLINK, (long)"/tmp/chmod01_file");
}

static void test_chmod01_dir(void) {
    puts("\n[ltp/chmod01-dir]\n");

    sc2(SYS_MKDIR, (long)"/tmp/chmod01_dir", 0755);

    int modes[] = {0, 07, 070, 0700, 0777, 02777, 04777, 06777};
    struct k_stat st;

    for (int i = 0; i < 8; i++) {
        long r = sc2(SYS_CHMOD, (long)"/tmp/chmod01_dir", modes[i]);
        check("chmod dir", r == 0);
        if (r != 0) continue;

        r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/chmod01_dir", (long)&st, 0);
        check("stat dir", r == 0);
        if (r == 0)
            check_val("mode bits", (long)(st.st_mode & 07777), (long)modes[i]);
    }

    sc1(SYS_RMDIR, (long)"/tmp/chmod01_dir");
}

/* ── chmod03: chmod with sticky bit (as owner) ── */
/* Original requires non-root. CosmoRT is single-user (root).
   We test the sticky-bit setting works from root. */

static void test_chmod03_file(void) {
    puts("\n[ltp/chmod03-file]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/chmod03_file", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    long r = sc2(SYS_CHMOD, (long)"/tmp/chmod03_file", 01777);
    check_val("chmod 01777", r, 0);

    struct k_stat st;
    r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/chmod03_file", (long)&st, 0);
    check("stat", r == 0);
    if (r == 0)
        check_val("sticky+perms", (long)(st.st_mode & 07777), 01777);

    sc1(SYS_UNLINK, (long)"/tmp/chmod03_file");
}

static void test_chmod03_dir(void) {
    puts("\n[ltp/chmod03-dir]\n");

    sc2(SYS_MKDIR, (long)"/tmp/chmod03_dir", 0777);

    long r = sc2(SYS_CHMOD, (long)"/tmp/chmod03_dir", 01777);
    check_val("chmod 01777", r, 0);

    struct k_stat st;
    r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/chmod03_dir", (long)&st, 0);
    check("stat", r == 0);
    if (r == 0)
        check_val("sticky+perms", (long)(st.st_mode & 07777), 01777);

    sc1(SYS_RMDIR, (long)"/tmp/chmod03_dir");
}

/* ── chmod05: setgid on dir owned by different group ── */
/* Original: non-root chmod with ISGID on dir where process gid != dir gid.
   Result: setgid cleared. CosmoRT single-user: root can set ISGID freely.
   We test that root CAN set ISGID (different semantics). */

static void test_chmod05(void) {
    puts("\n[ltp/chmod05]\n");
    puts("  NOTE  single-user: root can always set ISGID\n");

    sc2(SYS_MKDIR, (long)"/tmp/chmod05_dir", 0777);

    /* Set ISVTX | ISGID | all perms */
    long r = sc2(SYS_CHMOD, (long)"/tmp/chmod05_dir", 03777);
    check_val("chmod 03777", r, 0);

    struct k_stat st;
    r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/chmod05_dir", (long)&st, 0);
    check("stat", r == 0);
    if (r == 0)
        check_val("ISGID+ISVTX+perms", (long)(st.st_mode & 07777), 03777);

    sc1(SYS_RMDIR, (long)"/tmp/chmod05_dir");
}

/* ── chmod06: error cases ── */

static void test_chmod06_enoent(void) {
    puts("\n[ltp/chmod06-enoent]\n");

    long r = sc2(SYS_CHMOD, (long)"", 0644);
    check_val("chmod empty ENOENT", r, -ENOENT);

    r = sc2(SYS_CHMOD, (long)"/tmp/chmod06_nonexist", 0644);
    check_val("chmod nonexist ENOENT", r, -ENOENT);
}

static void test_chmod06_enotdir(void) {
    puts("\n[ltp/chmod06-enotdir]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/chmod06_notdir", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    long r = sc2(SYS_CHMOD, (long)"/tmp/chmod06_notdir/child", 0644);
    check_val("chmod through file ENOTDIR", r, -ENOTDIR);

    sc1(SYS_UNLINK, (long)"/tmp/chmod06_notdir");
}

static void test_chmod06_enametoolong(void) {
    puts("\n[ltp/chmod06-enametoolong]\n");

    /* Build a path longer than PATH_MAX (4096) */
    char long_path[4098];
    for (int i = 0; i < 4097; i++) long_path[i] = 'a';
    long_path[4097] = 0;

    long r = sc2(SYS_CHMOD, (long)long_path, 0644);
    check_val("chmod ENAMETOOLONG", r, -ENAMETOOLONG);
}

static void test_chmod06_eloop(void) {
    puts("\n[ltp/chmod06-eloop]\n");

    sc2(SYS_SYMLINK, (long)"/tmp/chmod06_loop2", (long)"/tmp/chmod06_loop1");
    sc2(SYS_SYMLINK, (long)"/tmp/chmod06_loop1", (long)"/tmp/chmod06_loop2");

    long r = sc2(SYS_CHMOD, (long)"/tmp/chmod06_loop1", 0644);
    check_val("chmod ELOOP", r, -ELOOP);

    sc1(SYS_UNLINK, (long)"/tmp/chmod06_loop1");
    sc1(SYS_UNLINK, (long)"/tmp/chmod06_loop2");
}

/* ── chmod07: root can set sticky bit on file owned by other ── */

static void test_chmod07(void) {
    puts("\n[ltp/chmod07]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/chmod07_file", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    long r = sc2(SYS_CHMOD, (long)"/tmp/chmod07_file", 01777);
    check_val("chmod 01777", r, 0);

    struct k_stat st;
    r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/chmod07_file", (long)&st, 0);
    check("stat", r == 0);
    if (r == 0)
        check_val("sticky+perms", (long)(st.st_mode & 07777), 01777);

    sc1(SYS_UNLINK, (long)"/tmp/chmod07_file");
}

/* ── chmod08: chmod on symlink target ── */

static void test_chmod08(void) {
    puts("\n[ltp/chmod08]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/chmod08_file", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    sc2(SYS_SYMLINK, (long)"/tmp/chmod08_file", (long)"/tmp/chmod08_link");

    /* chmod on symlink should change the target */
    long r = sc2(SYS_CHMOD, (long)"/tmp/chmod08_link", 01777);
    check_val("chmod via symlink", r, 0);

    struct k_stat st;
    r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/chmod08_file", (long)&st, 0);
    check("stat target", r == 0);
    if (r == 0)
        check_val("target mode", (long)(st.st_mode & 07777), 01777);

    sc1(SYS_UNLINK, (long)"/tmp/chmod08_link");
    sc1(SYS_UNLINK, (long)"/tmp/chmod08_file");
}

TEST("ltp/chmod01-file",       test_chmod01_file);
TEST("ltp/chmod01-dir",        test_chmod01_dir);
TEST("ltp/chmod03-file",       test_chmod03_file);
TEST("ltp/chmod03-dir",        test_chmod03_dir);
TEST("ltp/chmod05",            test_chmod05);
TEST("ltp/chmod06-enoent",     test_chmod06_enoent);
TEST("ltp/chmod06-enotdir",    test_chmod06_enotdir);
TEST("ltp/chmod06-enametoolong", test_chmod06_enametoolong);
TEST("ltp/chmod06-eloop",      test_chmod06_eloop);
TEST("ltp/chmod07",            test_chmod07);
TEST("ltp/chmod08",            test_chmod08);
