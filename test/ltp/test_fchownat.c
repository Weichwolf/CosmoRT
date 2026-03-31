/* LTP fchownat01-03 — ported to ktest */
#include "ktest.h"

/* ── fchownat01: basic fchownat with AT_FDCWD and dirfd ── */

static void test_fchownat01_atcwd(void) {
    puts("\n[ltp/fchownat01-atcwd]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchownat01_f1", O_CREAT | O_RDWR, 0600);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    sc1(SYS_CHDIR, (long)"/tmp");

    long r = sc5(SYS_FCHOWNAT, AT_FDCWD, (long)"fchownat01_f1", 1000, 1000, 0);
    check_val("fchownat AT_FDCWD", r, 0);

    struct k_stat st;
    r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"fchownat01_f1", (long)&st, 0);
    check("stat", r == 0);
    if (r == 0) {
        check_val("uid", (long)st.st_uid, 1000);
        check_val("gid", (long)st.st_gid, 1000);
    }

    sc1(SYS_UNLINK, (long)"/tmp/fchownat01_f1");
}

static void test_fchownat01_dirfd(void) {
    puts("\n[ltp/fchownat01-dirfd]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchownat01_f2", O_CREAT | O_RDWR, 0600);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    long dirfd = sc3(SYS_OPEN, (long)"/tmp", O_RDONLY | O_DIRECTORY, 0);
    check("open dir", dirfd >= 0);
    if (dirfd < 0) { sc1(SYS_UNLINK, (long)"/tmp/fchownat01_f2"); return; }

    long r = sc5(SYS_FCHOWNAT, dirfd, (long)"fchownat01_f2", 1000, 1000, 0);
    check_val("fchownat dirfd", r, 0);

    struct k_stat st;
    r = sc4(SYS_FSTATAT, dirfd, (long)"fchownat01_f2", (long)&st, 0);
    check("stat", r == 0);
    if (r == 0) {
        check_val("uid", (long)st.st_uid, 1000);
        check_val("gid", (long)st.st_gid, 1000);
    }

    sc1(SYS_CLOSE, dirfd);
    sc1(SYS_UNLINK, (long)"/tmp/fchownat01_f2");
}

/* ── fchownat02: AT_SYMLINK_NOFOLLOW operates on symlink itself ── */

static void test_fchownat02(void) {
    puts("\n[ltp/fchownat02]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchownat02_target", O_CREAT | O_RDWR, 0600);
    check("create target", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    sc2(SYS_SYMLINK, (long)"/tmp/fchownat02_target", (long)"/tmp/fchownat02_link");

    /* chown the symlink itself, not the target */
    long r = sc5(SYS_FCHOWNAT, AT_FDCWD,
                 (long)"/tmp/fchownat02_link", 1000, 1000,
                 AT_SYMLINK_NOFOLLOW);
    check_val("fchownat NOFOLLOW", r, 0);

    /* Stat the target — should NOT have uid=1000 */
    struct k_stat st_target;
    r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/fchownat02_target", (long)&st_target, 0);
    check("stat target", r == 0);

    /* Lstat the symlink — should have uid=1000 */
    struct k_stat st_link;
    r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/fchownat02_link",
            (long)&st_link, AT_SYMLINK_NOFOLLOW);
    check("lstat link", r == 0);

    if (r == 0) {
        check_val("link uid", (long)st_link.st_uid, 1000);
        check_val("link gid", (long)st_link.st_gid, 1000);
        check("target uid unchanged", st_target.st_uid != 1000);
    }

    sc1(SYS_UNLINK, (long)"/tmp/fchownat02_link");
    sc1(SYS_UNLINK, (long)"/tmp/fchownat02_target");
}

/* ── fchownat03: error cases ── */

static void test_fchownat03_ebadf(void) {
    puts("\n[ltp/fchownat03-ebadf]\n");

    long r = sc5(SYS_FCHOWNAT, -1, (long)"file", 0, 0, 0);
    check_val("fchownat EBADF", r, -EBADF);
}

static void test_fchownat03_enotdir(void) {
    puts("\n[ltp/fchownat03-enotdir]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchownat03_notdir", O_CREAT | O_RDWR, 0600);
    check("create", fd >= 0);
    if (fd < 0) return;

    long r = sc5(SYS_FCHOWNAT, fd, (long)"file", 0, 0, 0);
    check_val("fchownat ENOTDIR", r, -ENOTDIR);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fchownat03_notdir");
}

static void test_fchownat03_einval(void) {
    puts("\n[ltp/fchownat03-einval]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchownat03_file", O_CREAT | O_RDWR, 0600);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    long dirfd = sc3(SYS_OPEN, (long)"/tmp", O_RDONLY | O_DIRECTORY, 0);
    check("open dir", dirfd >= 0);
    if (dirfd < 0) { sc1(SYS_UNLINK, (long)"/tmp/fchownat03_file"); return; }

    long r = sc5(SYS_FCHOWNAT, dirfd, (long)"fchownat03_file", 0, 0, 9999);
    check_val("fchownat EINVAL", r, -EINVAL);

    sc1(SYS_CLOSE, dirfd);
    sc1(SYS_UNLINK, (long)"/tmp/fchownat03_file");
}

static void test_fchownat03_enoent(void) {
    puts("\n[ltp/fchownat03-enoent]\n");

    long r = sc5(SYS_FCHOWNAT, AT_FDCWD, (long)"/tmp/does/not/exist", 0, 0, 0);
    check_val("fchownat ENOENT", r, -ENOENT);
}

static void test_fchownat03_enametoolong(void) {
    puts("\n[ltp/fchownat03-enametoolong]\n");

    char long_path[4098];
    for (int i = 0; i < 4097; i++) long_path[i] = 'a';
    long_path[4097] = 0;

    long r = sc5(SYS_FCHOWNAT, AT_FDCWD, (long)long_path, 0, 0, 0);
    check_val("fchownat ENAMETOOLONG", r, -ENAMETOOLONG);
}

static void test_fchownat03_eloop(void) {
    puts("\n[ltp/fchownat03-eloop]\n");

    sc2(SYS_SYMLINK, (long)"/tmp/fchownat03_loop2", (long)"/tmp/fchownat03_loop1");
    sc2(SYS_SYMLINK, (long)"/tmp/fchownat03_loop1", (long)"/tmp/fchownat03_loop2");

    long r = sc5(SYS_FCHOWNAT, AT_FDCWD, (long)"/tmp/fchownat03_loop1", 0, 0, 0);
    check_val("fchownat ELOOP", r, -ELOOP);

    sc1(SYS_UNLINK, (long)"/tmp/fchownat03_loop1");
    sc1(SYS_UNLINK, (long)"/tmp/fchownat03_loop2");
}

TEST("ltp/fchownat01-atcwd",       test_fchownat01_atcwd);
TEST("ltp/fchownat01-dirfd",       test_fchownat01_dirfd);
TEST("ltp/fchownat02",             test_fchownat02);
TEST("ltp/fchownat03-ebadf",       test_fchownat03_ebadf);
TEST("ltp/fchownat03-enotdir",     test_fchownat03_enotdir);
TEST("ltp/fchownat03-einval",      test_fchownat03_einval);
TEST("ltp/fchownat03-enoent",      test_fchownat03_enoent);
TEST("ltp/fchownat03-enametoolong", test_fchownat03_enametoolong);
TEST("ltp/fchownat03-eloop",       test_fchownat03_eloop);
