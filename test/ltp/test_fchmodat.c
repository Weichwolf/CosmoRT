/* LTP fchmodat01/fchmodat02/fchmodat2_02 — ported to ktest */
#include "ktest.h"

/* ── fchmodat01: basic fchmodat with dirfd, AT_FDCWD, absolute path ── */

static void test_fchmodat01_dirfd(void) {
    puts("\n[ltp/fchmodat01-dirfd]\n");

    sc2(SYS_MKDIR, (long)"/tmp/fchmodat01_dir", 0700);
    long fd = sc3(SYS_OPEN, (long)"/tmp/fchmodat01_dir/file",
                  O_CREAT | O_RDWR, 0644);
    check("create file", fd >= 0);
    if (fd < 0) { sc1(SYS_RMDIR, (long)"/tmp/fchmodat01_dir"); return; }
    sc1(SYS_CLOSE, fd);

    long dirfd = sc3(SYS_OPEN, (long)"/tmp/fchmodat01_dir", O_RDONLY | O_DIRECTORY, 0);
    check("open dir", dirfd >= 0);
    if (dirfd < 0) goto cleanup;

    long r = sc4(SYS_FCHMODAT, dirfd, (long)"file", 0600, 0);
    check_val("fchmodat dirfd", r, 0);

    struct k_stat st;
    r = sc4(SYS_FSTATAT, dirfd, (long)"file", (long)&st, 0);
    check("stat", r == 0);
    if (r == 0)
        check_val("mode", (long)(st.st_mode & 07777), 0600);

    sc1(SYS_CLOSE, dirfd);
cleanup:
    sc1(SYS_UNLINK, (long)"/tmp/fchmodat01_dir/file");
    sc1(SYS_RMDIR, (long)"/tmp/fchmodat01_dir");
}

static void test_fchmodat01_atcwd(void) {
    puts("\n[ltp/fchmodat01-atcwd]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchmodat01_cwd", O_CREAT | O_RDWR, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    /* chdir to /tmp first */
    sc1(SYS_CHDIR, (long)"/tmp");

    long r = sc4(SYS_FCHMODAT, AT_FDCWD, (long)"fchmodat01_cwd", 0600, 0);
    check_val("fchmodat AT_FDCWD", r, 0);

    struct k_stat st;
    r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"fchmodat01_cwd", (long)&st, 0);
    check("stat", r == 0);
    if (r == 0)
        check_val("mode", (long)(st.st_mode & 07777), 0600);

    sc1(SYS_UNLINK, (long)"/tmp/fchmodat01_cwd");
}

static void test_fchmodat01_abs(void) {
    puts("\n[ltp/fchmodat01-abs]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchmodat01_abs", O_CREAT | O_RDWR, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    /* With absolute path, dirfd is ignored — use bogus fd */
    long r = sc4(SYS_FCHMODAT, 9999, (long)"/tmp/fchmodat01_abs", 0600, 0);
    check_val("fchmodat abs path", r, 0);

    struct k_stat st;
    r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/fchmodat01_abs", (long)&st, 0);
    check("stat", r == 0);
    if (r == 0)
        check_val("mode", (long)(st.st_mode & 07777), 0600);

    sc1(SYS_UNLINK, (long)"/tmp/fchmodat01_abs");
}

/* ── fchmodat02: error cases ── */

static void test_fchmodat02_enotdir(void) {
    puts("\n[ltp/fchmodat02-enotdir]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchmodat02_notdir", O_CREAT | O_RDWR, 0600);
    check("create", fd >= 0);
    if (fd < 0) return;

    /* Use file fd as dirfd — should get ENOTDIR */
    long r = sc4(SYS_FCHMODAT, fd, (long)"file", 0600, 0);
    check_val("fchmodat ENOTDIR", r, -ENOTDIR);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fchmodat02_notdir");
}

static void test_fchmodat02_ebadf(void) {
    puts("\n[ltp/fchmodat02-ebadf]\n");

    long r = sc4(SYS_FCHMODAT, -1, (long)"file", 0600, 0);
    check_val("fchmodat EBADF", r, -EBADF);
}

static void test_fchmodat02_enametoolong(void) {
    puts("\n[ltp/fchmodat02-enametoolong]\n");

    char long_path[4098];
    for (int i = 0; i < 4097; i++) long_path[i] = 'a';
    long_path[4097] = 0;

    long r = sc4(SYS_FCHMODAT, AT_FDCWD, (long)long_path, 0600, 0);
    check_val("fchmodat ENAMETOOLONG", r, -ENAMETOOLONG);
}

static void test_fchmodat02_enoent(void) {
    puts("\n[ltp/fchmodat02-enoent]\n");

    long r = sc4(SYS_FCHMODAT, AT_FDCWD, (long)"", 0600, 0);
    check_val("fchmodat empty ENOENT", r, -ENOENT);
}

/* ── fchmodat2_02: error cases for fchmodat2 ── */

static void test_fchmodat2_ebadf(void) {
    puts("\n[ltp/fchmodat2-ebadf]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchmodat2_file", O_CREAT | O_RDWR, 0640);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    long r = sc4(SYS_FCHMODAT2, -1, (long)"fchmodat2_file", 0777, 0);
    check_val("fchmodat2 EBADF", r, -EBADF);

    sc1(SYS_UNLINK, (long)"/tmp/fchmodat2_file");
}

static void test_fchmodat2_enoent(void) {
    puts("\n[ltp/fchmodat2-enoent]\n");

    sc2(SYS_MKDIR, (long)"/tmp/fchmodat2_dir", 0700);
    long dirfd = sc3(SYS_OPEN, (long)"/tmp/fchmodat2_dir", O_RDONLY | O_DIRECTORY, 0);
    check("open dir", dirfd >= 0);
    if (dirfd < 0) { sc1(SYS_RMDIR, (long)"/tmp/fchmodat2_dir"); return; }

    long r = sc4(SYS_FCHMODAT2, dirfd, (long)"doesnt_exist.txt", 0777, 0);
    check_val("fchmodat2 ENOENT", r, -ENOENT);

    sc1(SYS_CLOSE, dirfd);
    sc1(SYS_RMDIR, (long)"/tmp/fchmodat2_dir");
}

static void test_fchmodat2_einval(void) {
    puts("\n[ltp/fchmodat2-einval]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchmodat2_inv", O_CREAT | O_RDWR, 0640);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    sc1(SYS_CHDIR, (long)"/tmp");

    long r = sc4(SYS_FCHMODAT2, AT_FDCWD, (long)"fchmodat2_inv", 0777, -1);
    check_val("fchmodat2 EINVAL", r, -EINVAL);

    sc1(SYS_UNLINK, (long)"/tmp/fchmodat2_inv");
}

TEST("ltp/fchmodat01-dirfd",       test_fchmodat01_dirfd);
TEST("ltp/fchmodat01-atcwd",       test_fchmodat01_atcwd);
TEST("ltp/fchmodat01-abs",         test_fchmodat01_abs);
TEST("ltp/fchmodat02-enotdir",     test_fchmodat02_enotdir);
TEST("ltp/fchmodat02-ebadf",       test_fchmodat02_ebadf);
TEST("ltp/fchmodat02-enametoolong", test_fchmodat02_enametoolong);
TEST("ltp/fchmodat02-enoent",      test_fchmodat02_enoent);
TEST("ltp/fchmodat2-ebadf",        test_fchmodat2_ebadf);
TEST("ltp/fchmodat2-enoent",       test_fchmodat2_enoent);
TEST("ltp/fchmodat2-einval",       test_fchmodat2_einval);
