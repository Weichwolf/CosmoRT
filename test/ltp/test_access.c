/* LTP access01/access02/access03/access04 — ported to ktest */
#include "ktest.h"

#define PATH_MAX 4096

/* ── access01: basic F_OK/R_OK/W_OK/X_OK on existing file ── */

static void create_file(const char *path, int mode) {
    long fd = sc3(SYS_OPEN, (long)path, O_CREAT | O_WRONLY | O_TRUNC, mode);
    if (fd >= 0) sc1(SYS_CLOSE, fd);
}

static void test_access01_fok(void) {
    puts("\n[ltp/access01-fok]\n");
    create_file("/tmp/acc_rwx", 0777);
    long r = sc2(SYS_ACCESS, (long)"/tmp/acc_rwx", F_OK);
    check_val("access F_OK", r, 0);
    sc1(SYS_UNLINK, (long)"/tmp/acc_rwx");
}

static void test_access01_rok(void) {
    puts("\n[ltp/access01-rok]\n");
    create_file("/tmp/acc_rwx", 0777);
    long r = sc2(SYS_ACCESS, (long)"/tmp/acc_rwx", R_OK);
    check_val("access R_OK", r, 0);
    sc1(SYS_UNLINK, (long)"/tmp/acc_rwx");
}

static void test_access01_wok(void) {
    puts("\n[ltp/access01-wok]\n");
    create_file("/tmp/acc_rwx", 0777);
    long r = sc2(SYS_ACCESS, (long)"/tmp/acc_rwx", W_OK);
    check_val("access W_OK", r, 0);
    sc1(SYS_UNLINK, (long)"/tmp/acc_rwx");
}

static void test_access01_xok(void) {
    puts("\n[ltp/access01-xok]\n");
    create_file("/tmp/acc_rwx", 0777);
    long r = sc2(SYS_ACCESS, (long)"/tmp/acc_rwx", X_OK);
    check_val("access X_OK", r, 0);
    sc1(SYS_UNLINK, (long)"/tmp/acc_rwx");
}

static void test_access01_combined(void) {
    puts("\n[ltp/access01-combined]\n");
    create_file("/tmp/acc_rwx", 0777);

    long r = sc2(SYS_ACCESS, (long)"/tmp/acc_rwx", R_OK | W_OK);
    check_val("access R_OK|W_OK", r, 0);

    r = sc2(SYS_ACCESS, (long)"/tmp/acc_rwx", R_OK | X_OK);
    check_val("access R_OK|X_OK", r, 0);

    r = sc2(SYS_ACCESS, (long)"/tmp/acc_rwx", R_OK | W_OK | X_OK);
    check_val("access R_OK|W_OK|X_OK", r, 0);

    sc1(SYS_UNLINK, (long)"/tmp/acc_rwx");
}

/* ── access02: error cases ── */

static void test_access02_enoent(void) {
    puts("\n[ltp/access02-enoent]\n");
    long r = sc2(SYS_ACCESS, (long)"/tmp/nonexistent_acc_file", W_OK);
    check_val("access ENOENT", r, -ENOENT);
}

static void test_access02_enametoolong(void) {
    puts("\n[ltp/access02-enametoolong]\n");
    /* Build a path longer than PATH_MAX */
    char longpath[PATH_MAX + 8];
    for (int i = 0; i < PATH_MAX + 2; i++)
        longpath[i] = 'a';
    longpath[PATH_MAX + 2] = 0;

    long r = sc2(SYS_ACCESS, (long)longpath, R_OK);
    check_val("access ENAMETOOLONG", r, -ENAMETOOLONG);
}

static void test_access02_enotdir(void) {
    puts("\n[ltp/access02-enotdir]\n");
    /* /dev/null is a file, not a dir — path through it should be ENOTDIR */
    create_file("/tmp/acc_notdir", 0644);
    long r = sc2(SYS_ACCESS, (long)"/tmp/acc_notdir/child", R_OK);
    check_val("access ENOTDIR", r, -ENOTDIR);
    sc1(SYS_UNLINK, (long)"/tmp/acc_notdir");
}

static void test_access02_eloop(void) {
    puts("\n[ltp/access02-eloop]\n");
    /* Create symlink loop: sym1 -> sym2 -> sym1 */
    sc1(SYS_UNLINK, (long)"/tmp/acc_sym1");
    sc1(SYS_UNLINK, (long)"/tmp/acc_sym2");
    sc2(SYS_SYMLINK, (long)"/tmp/acc_sym2", (long)"/tmp/acc_sym1");
    sc2(SYS_SYMLINK, (long)"/tmp/acc_sym1", (long)"/tmp/acc_sym2");

    long r = sc2(SYS_ACCESS, (long)"/tmp/acc_sym1", R_OK);
    check_val("access ELOOP", r, -ELOOP);

    sc1(SYS_UNLINK, (long)"/tmp/acc_sym1");
    sc1(SYS_UNLINK, (long)"/tmp/acc_sym2");
}

/* ── access03: faccessat with AT_FDCWD ── */

static void test_access03_faccessat(void) {
    puts("\n[ltp/access03-faccessat]\n");
    create_file("/tmp/acc_fat", 0644);

    long r = sc3(SYS_FACCESSAT, AT_FDCWD, (long)"/tmp/acc_fat", F_OK);
    check_val("faccessat F_OK", r, 0);

    r = sc3(SYS_FACCESSAT, AT_FDCWD, (long)"/tmp/acc_fat", R_OK);
    check_val("faccessat R_OK", r, 0);

    r = sc3(SYS_FACCESSAT, AT_FDCWD, (long)"/tmp/acc_fat", W_OK);
    check_val("faccessat W_OK", r, 0);

    sc1(SYS_UNLINK, (long)"/tmp/acc_fat");
}

static void test_access03_faccessat_enoent(void) {
    puts("\n[ltp/access03-faccessat-enoent]\n");
    long r = sc3(SYS_FACCESSAT, AT_FDCWD, (long)"/tmp/acc_fat_noexist", F_OK);
    check_val("faccessat ENOENT", r, -ENOENT);
}

/* ── access04: X_OK on non-executable returns EACCES ── */

static void test_access04_xok_noperm(void) {
    puts("\n[ltp/access04-xok-noperm]\n");
    /* File with mode 0644 — no execute bit */
    create_file("/tmp/acc_noexec", 0644);
    long r = sc2(SYS_ACCESS, (long)"/tmp/acc_noexec", X_OK);
    check_val("access X_OK no-exec EACCES", r, -EACCES);
    sc1(SYS_UNLINK, (long)"/tmp/acc_noexec");
}

static void test_access04_rok_noread(void) {
    puts("\n[ltp/access04-rok-noread]\n");
    /* File with mode 0222 — no read bit */
    create_file("/tmp/acc_noread", 0222);
    long r = sc2(SYS_ACCESS, (long)"/tmp/acc_noread", R_OK);
    /* As root, this may succeed. Still test it. */
    check("access R_OK on 0222 (root may pass)", r == 0 || r == -EACCES);
    sc1(SYS_UNLINK, (long)"/tmp/acc_noread");
}

/* ── access01 DAC via setuid(nobody): file owned by root, nobody-access ── */

#define NOBODY_UID 65534U

static void make_file_mode(const char *path, int mode) {
    /* umask-proof: open then chmod to exact mode */
    sc1(SYS_UNLINK, (long)path);
    long fd = sc3(SYS_OPEN, (long)path, O_CREAT | O_WRONLY, 0600);
    if (fd >= 0) sc1(SYS_CLOSE, fd);
    sc2(SYS_CHMOD, (long)path, mode);
}

static void child_acc01_r_only(void) {
    long r = sc1(SYS_SETUID, NOBODY_UID);
    check_val("setuid(nobody)", r, 0);
    /* 0400 file: other=0, nobody gets nothing */
    r = sc2(SYS_ACCESS, (long)"/tmp/acc_dac_r", R_OK);
    check_val("nobody R_OK on 0400 root file", r, -EACCES);
    r = sc2(SYS_ACCESS, (long)"/tmp/acc_dac_r", W_OK);
    check_val("nobody W_OK on 0400 root file", r, -EACCES);
    r = sc2(SYS_ACCESS, (long)"/tmp/acc_dac_r", X_OK);
    check_val("nobody X_OK on 0400 root file", r, -EACCES);
    r = sc2(SYS_ACCESS, (long)"/tmp/acc_dac_r", F_OK);
    check_val("nobody F_OK on 0400 root file", r, 0);
}

static void test_access01_dac_0400(void) {
    puts("\n[ltp/access01-dac-0400]\n");
    make_file_mode("/tmp/acc_dac_r", 0400);
    long pid = sc0(SYS_FORK);
    if (pid == 0) { child_acc01_r_only(); sc1(SYS_EXIT, 0); for(;;){} }
    int st=0; sc4(SYS_WAIT4, pid, (long)&st, 0, 0);
    sc1(SYS_UNLINK, (long)"/tmp/acc_dac_r");
}

static void child_acc01_rwx_as_other(void) {
    sc1(SYS_SETUID, NOBODY_UID);
    /* 0777: everybody all perms */
    long r = sc2(SYS_ACCESS, (long)"/tmp/acc_dac_rwx", R_OK);
    check_val("nobody R_OK on 0777", r, 0);
    r = sc2(SYS_ACCESS, (long)"/tmp/acc_dac_rwx", W_OK);
    check_val("nobody W_OK on 0777", r, 0);
    r = sc2(SYS_ACCESS, (long)"/tmp/acc_dac_rwx", X_OK);
    check_val("nobody X_OK on 0777", r, 0);
    r = sc2(SYS_ACCESS, (long)"/tmp/acc_dac_rwx", R_OK | W_OK | X_OK);
    check_val("nobody R|W|X on 0777", r, 0);
}

static void test_access01_dac_0777(void) {
    puts("\n[ltp/access01-dac-0777]\n");
    make_file_mode("/tmp/acc_dac_rwx", 0777);
    long pid = sc0(SYS_FORK);
    if (pid == 0) { child_acc01_rwx_as_other(); sc1(SYS_EXIT, 0); for(;;){} }
    int st=0; sc4(SYS_WAIT4, pid, (long)&st, 0, 0);
    sc1(SYS_UNLINK, (long)"/tmp/acc_dac_rwx");
}

/* ── access01 mode 0100 x-only as nobody — R_OK/W_OK EACCES, X_OK ok ── */

static void child_acc01_x_only(void) {
    sc1(SYS_SETUID, NOBODY_UID);
    long r = sc2(SYS_ACCESS, (long)"/tmp/acc_dac_x", R_OK);
    check_val("nobody R_OK on 0111", r, -EACCES);
    r = sc2(SYS_ACCESS, (long)"/tmp/acc_dac_x", W_OK);
    check_val("nobody W_OK on 0111", r, -EACCES);
    r = sc2(SYS_ACCESS, (long)"/tmp/acc_dac_x", X_OK);
    check_val("nobody X_OK on 0111", r, 0);
}

static void test_access01_dac_0111(void) {
    puts("\n[ltp/access01-dac-0111]\n");
    make_file_mode("/tmp/acc_dac_x", 0111);
    long pid = sc0(SYS_FORK);
    if (pid == 0) { child_acc01_x_only(); sc1(SYS_EXIT, 0); for(;;){} }
    int st=0; sc4(SYS_WAIT4, pid, (long)&st, 0, 0);
    sc1(SYS_UNLINK, (long)"/tmp/acc_dac_x");
}

/* ── access04 EINVAL: invalid mode bits ── */

static void test_access04_einval(void) {
    puts("\n[ltp/access04-einval]\n");
    create_file("/tmp/acc_einval", 0644);
    long r = sc2(SYS_ACCESS, (long)"/tmp/acc_einval", -1);
    check_val("access mode=-1 EINVAL", r, -EINVAL);
    sc1(SYS_UNLINK, (long)"/tmp/acc_einval");
}

/* ── access04 root X_OK on non-exec regular file ── */

static void test_access04_root_xok(void) {
    puts("\n[ltp/access04-root-xok]\n");
    create_file("/tmp/acc_root_noexec", 0666);
    long r = sc2(SYS_ACCESS, (long)"/tmp/acc_root_noexec", X_OK);
    check_val("root X_OK on 0666 regular EACCES", r, -EACCES);
    sc1(SYS_UNLINK, (long)"/tmp/acc_root_noexec");
}

TEST("ltp/access01-fok",          test_access01_fok);
TEST("ltp/access01-rok",          test_access01_rok);
TEST("ltp/access01-wok",          test_access01_wok);
TEST("ltp/access01-xok",          test_access01_xok);
TEST("ltp/access01-combined",     test_access01_combined);
TEST("ltp/access02-enoent",       test_access02_enoent);
TEST("ltp/access02-enametoolong", test_access02_enametoolong);
TEST("ltp/access02-enotdir",      test_access02_enotdir);
TEST("ltp/access02-eloop",        test_access02_eloop);
TEST("ltp/access03-faccessat",    test_access03_faccessat);
TEST("ltp/access03-faccessat-enoent", test_access03_faccessat_enoent);
TEST("ltp/access04-xok-noperm",   test_access04_xok_noperm);
TEST("ltp/access04-rok-noread",   test_access04_rok_noread);
TEST("ltp/access01-dac-0400",     test_access01_dac_0400);
TEST("ltp/access01-dac-0777",     test_access01_dac_0777);
TEST("ltp/access01-dac-0111",     test_access01_dac_0111);
TEST("ltp/access04-einval",       test_access04_einval);
TEST("ltp/access04-root-xok",     test_access04_root_xok);
