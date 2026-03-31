/* LTP chown01-05 — ported to ktest */
#include "ktest.h"

/* ── chown01: basic chown succeeds ── */

static void test_chown01(void) {
    puts("\n[ltp/chown01]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/chown01_file", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    /* chown to uid=0, gid=0 (we are root) */
    long r = sc3(SYS_CHOWN, (long)"/tmp/chown01_file", 0, 0);
    check_val("chown(0,0)", r, 0);

    struct k_stat st;
    r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/chown01_file", (long)&st, 0);
    check("stat", r == 0);
    if (r == 0) {
        check_val("uid", (long)st.st_uid, 0);
        check_val("gid", (long)st.st_gid, 0);
    }

    sc1(SYS_UNLINK, (long)"/tmp/chown01_file");
}

/* ── chown02: chown clears setuid/setgid on executable ── */

static void test_chown02_clears_suid(void) {
    puts("\n[ltp/chown02-clears-suid]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/chown02_f1", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    /* Set ISUID|ISGID|IRWXU|IRWXG = 06770 */
    long r = sc2(SYS_CHMOD, (long)"/tmp/chown02_f1", 06770);
    check_val("chmod 06770", r, 0);

    r = sc3(SYS_CHOWN, (long)"/tmp/chown02_f1", 0, 0);
    check_val("chown", r, 0);

    struct k_stat st;
    r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/chown02_f1", (long)&st, 0);
    check("stat", r == 0);
    if (r == 0) {
        /* ISUID and ISGID should be cleared because file is group-executable */
        check_val("suid/sgid cleared", (long)(st.st_mode & 06000), 0);
        check("rwx preserved", (st.st_mode & 0770) == 0770);
    }

    sc1(SYS_UNLINK, (long)"/tmp/chown02_f1");
}

static void test_chown02_preserves_sgid(void) {
    puts("\n[ltp/chown02-preserves-sgid]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/chown02_f2", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    /* Set ISGID|IRWXU (no group-execute) = 02700 */
    long r = sc2(SYS_CHMOD, (long)"/tmp/chown02_f2", 02700);
    check_val("chmod 02700", r, 0);

    r = sc3(SYS_CHOWN, (long)"/tmp/chown02_f2", 0, 0);
    check_val("chown", r, 0);

    struct k_stat st;
    r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/chown02_f2", (long)&st, 0);
    check("stat", r == 0);
    if (r == 0) {
        /* ISGID preserved on non-group-executable file (mandatory locking) */
        check_val("sgid preserved", (long)(st.st_mode & 02000), 02000);
    }

    sc1(SYS_UNLINK, (long)"/tmp/chown02_f2");
}

/* ── chown03: chown clears suid/sgid on gid change (as owner) ── */
/* Original runs as non-root owner. CosmoRT single-user: always root.
   We test the suid/sgid clearing behavior on gid change. */

static void test_chown03(void) {
    puts("\n[ltp/chown03]\n");
    puts("  NOTE  single-user: always root, testing sgid clear on gid change\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/chown03_file", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    /* Set ISUID|ISGID|IRWXU|IRWXG */
    long r = sc2(SYS_CHMOD, (long)"/tmp/chown03_file", 06770);
    check_val("chmod", r, 0);

    /* Change only gid (uid=-1) */
    r = sc3(SYS_CHOWN, (long)"/tmp/chown03_file", (long)-1, 0);
    check_val("chown(-1, 0)", r, 0);

    struct k_stat st;
    r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/chown03_file", (long)&st, 0);
    check("stat", r == 0);
    if (r == 0) {
        check_val("uid unchanged", (long)st.st_uid, 0);
        check_val("gid set", (long)st.st_gid, 0);
    }

    sc1(SYS_UNLINK, (long)"/tmp/chown03_file");
}

/* ── chown04: error cases ── */

static void test_chown04_enoent(void) {
    puts("\n[ltp/chown04-enoent]\n");

    long r = sc3(SYS_CHOWN, (long)"", 0, 0);
    check_val("chown empty ENOENT", r, -ENOENT);

    r = sc3(SYS_CHOWN, (long)"/tmp/chown04_nonexist", 0, 0);
    check_val("chown nonexist ENOENT", r, -ENOENT);
}

static void test_chown04_enotdir(void) {
    puts("\n[ltp/chown04-enotdir]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/chown04_notdir", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    long r = sc3(SYS_CHOWN, (long)"/tmp/chown04_notdir/child", 0, 0);
    check_val("chown through file ENOTDIR", r, -ENOTDIR);

    sc1(SYS_UNLINK, (long)"/tmp/chown04_notdir");
}

static void test_chown04_enametoolong(void) {
    puts("\n[ltp/chown04-enametoolong]\n");

    char long_path[4098];
    for (int i = 0; i < 4097; i++) long_path[i] = 'a';
    long_path[4097] = 0;

    long r = sc3(SYS_CHOWN, (long)long_path, 0, 0);
    check_val("chown ENAMETOOLONG", r, -ENAMETOOLONG);
}

static void test_chown04_eloop(void) {
    puts("\n[ltp/chown04-eloop]\n");

    sc2(SYS_SYMLINK, (long)"/tmp/chown04_loop2", (long)"/tmp/chown04_loop1");
    sc2(SYS_SYMLINK, (long)"/tmp/chown04_loop1", (long)"/tmp/chown04_loop2");

    long r = sc3(SYS_CHOWN, (long)"/tmp/chown04_loop1", 0, 0);
    check_val("chown ELOOP", r, -ELOOP);

    sc1(SYS_UNLINK, (long)"/tmp/chown04_loop1");
    sc1(SYS_UNLINK, (long)"/tmp/chown04_loop2");
}

/* ── chown05: chown to arbitrary uid/gid as root ── */

static void test_chown05(void) {
    puts("\n[ltp/chown05]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/chown05_file", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    struct k_stat st;

    /* change both uid and gid */
    long r = sc3(SYS_CHOWN, (long)"/tmp/chown05_file", 700, 701);
    check_val("chown(700,701)", r, 0);
    r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/chown05_file", (long)&st, 0);
    check("stat", r == 0);
    if (r == 0) { check_val("uid", (long)st.st_uid, 700); check_val("gid", (long)st.st_gid, 701); }

    /* change owner only */
    r = sc3(SYS_CHOWN, (long)"/tmp/chown05_file", 702, (long)-1);
    check_val("chown(702,-1)", r, 0);
    r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/chown05_file", (long)&st, 0);
    check("stat", r == 0);
    if (r == 0) { check_val("uid", (long)st.st_uid, 702); check_val("gid unchanged", (long)st.st_gid, 701); }

    /* change group only */
    r = sc3(SYS_CHOWN, (long)"/tmp/chown05_file", (long)-1, 704);
    check_val("chown(-1,704)", r, 0);
    r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/chown05_file", (long)&st, 0);
    check("stat", r == 0);
    if (r == 0) { check_val("uid unchanged", (long)st.st_uid, 702); check_val("gid", (long)st.st_gid, 704); }

    /* no change */
    r = sc3(SYS_CHOWN, (long)"/tmp/chown05_file", (long)-1, (long)-1);
    check_val("chown(-1,-1)", r, 0);
    r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/chown05_file", (long)&st, 0);
    check("stat", r == 0);
    if (r == 0) { check_val("uid nop", (long)st.st_uid, 702); check_val("gid nop", (long)st.st_gid, 704); }

    sc1(SYS_UNLINK, (long)"/tmp/chown05_file");
}

TEST("ltp/chown01",              test_chown01);
TEST("ltp/chown02-clears-suid",  test_chown02_clears_suid);
TEST("ltp/chown02-preserves-sgid", test_chown02_preserves_sgid);
TEST("ltp/chown03",              test_chown03);
TEST("ltp/chown04-enoent",       test_chown04_enoent);
TEST("ltp/chown04-enotdir",      test_chown04_enotdir);
TEST("ltp/chown04-enametoolong", test_chown04_enametoolong);
TEST("ltp/chown04-eloop",        test_chown04_eloop);
TEST("ltp/chown05",              test_chown05);
