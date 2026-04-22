/* LTP chroot01-04 — ported to ktest */
#include "ktest.h"

#define LINUX_CAPABILITY_VERSION_3 0x20080522
#define CAP_SYS_CHROOT_BIT 18

struct cap_header { uint32_t version; int pid; };
struct cap_data   { uint32_t effective, permitted, inheritable; };

/* ── chroot01: EPERM when CAP_SYS_CHROOT is not in pE ──
 * Fork a child, drop CAP_SYS_CHROOT via capset(), verify chroot → EPERM. */

static void test_chroot01(void) {
    puts("\n[ltp/chroot01]\n");

    sc2(SYS_MKDIR, (long)"/tmp/chroot01_root", 0755);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct cap_header hdr = { .version = LINUX_CAPABILITY_VERSION_3, .pid = 0 };
        struct cap_data data[2] = {0};
        sc2(SYS_CAPGET, (long)&hdr, (long)data);
        data[0].effective &= ~(1u << CAP_SYS_CHROOT_BIT);
        long cs = sc2(SYS_CAPSET, (long)&hdr, (long)data);
        if (cs != 0) sc1(SYS_EXIT, 10);

        long r = sc1(SYS_CHROOT, (long)"/tmp/chroot01_root");
        sc1(SYS_EXIT, r == -EPERM ? 0 : 20);
    }
    check("fork", pid > 0);
    if (pid > 0) {
        int status = 0;
        sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
        int code = (status >> 8) & 0xFF;
        check_val("EPERM on dropped CAP_SYS_CHROOT", (long)code, 0);
    }
    sc1(SYS_RMDIR, (long)"/tmp/chroot01_root");
}

/* ── chroot02: basic chroot + stat file in new root ── */
/* Original forks a child. We do it inline since ktest is single-process. */

static void test_chroot02(void) {
    puts("\n[ltp/chroot02]\n");

    /* Create a directory and file to use as new root */
    sc2(SYS_MKDIR, (long)"/tmp/chroot02_root", 0755);
    long fd = sc3(SYS_OPEN, (long)"/tmp/chroot02_root/testfile",
                  O_CREAT | O_RDWR, 0666);
    check("create testfile", fd >= 0);
    if (fd < 0) { sc1(SYS_RMDIR, (long)"/tmp/chroot02_root"); return; }
    sc1(SYS_CLOSE, fd);

    /* Fork to isolate the chroot effect */
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Child: chroot and verify */
        long r = sc1(SYS_CHROOT, (long)"/tmp/chroot02_root");
        if (r != 0) sc1(SYS_EXIT, 1);

        struct k_stat st;
        r = sc4(SYS_FSTATAT, AT_FDCWD, (long)"/testfile", (long)&st, 0);
        sc1(SYS_EXIT, r == 0 ? 0 : 2);
    }
    check("fork", pid > 0);

    if (pid > 0) {
        int status = 0;
        sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
        /* WIFEXITED && WEXITSTATUS == 0 */
        int exited = ((status & 0x7F) == 0);
        int code = (status >> 8) & 0xFF;
        check("child exited", exited);
        check_val("child exit code", (long)code, 0);
    }

    sc1(SYS_UNLINK, (long)"/tmp/chroot02_root/testfile");
    sc1(SYS_RMDIR, (long)"/tmp/chroot02_root");
}

/* ── chroot03: error cases ── */

static void test_chroot03_enametoolong(void) {
    puts("\n[ltp/chroot03-enametoolong]\n");

    char long_path[4098];
    for (int i = 0; i < 4097; i++) long_path[i] = 'a';
    long_path[4097] = 0;

    long r = sc1(SYS_CHROOT, (long)long_path);
    check_val("chroot ENAMETOOLONG", r, -ENAMETOOLONG);
}

static void test_chroot03_enotdir(void) {
    puts("\n[ltp/chroot03-enotdir]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/chroot03_file", O_RDWR | O_CREAT, 0666);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    long r = sc1(SYS_CHROOT, (long)"/tmp/chroot03_file");
    check_val("chroot ENOTDIR", r, -ENOTDIR);

    sc1(SYS_UNLINK, (long)"/tmp/chroot03_file");
}

static void test_chroot03_enoent(void) {
    puts("\n[ltp/chroot03-enoent]\n");

    long r = sc1(SYS_CHROOT, (long)"/tmp/chroot03_nonexist");
    check_val("chroot ENOENT", r, -ENOENT);
}

static void test_chroot03_eloop(void) {
    puts("\n[ltp/chroot03-eloop]\n");

    sc2(SYS_SYMLINK, (long)"/tmp/chroot03_loop2", (long)"/tmp/chroot03_loop1");
    sc2(SYS_SYMLINK, (long)"/tmp/chroot03_loop1", (long)"/tmp/chroot03_loop2");

    long r = sc1(SYS_CHROOT, (long)"/tmp/chroot03_loop1");
    check_val("chroot ELOOP", r, -ELOOP);

    sc1(SYS_UNLINK, (long)"/tmp/chroot03_loop1");
    sc1(SYS_UNLINK, (long)"/tmp/chroot03_loop2");
}

/* ── chroot04: EACCES for directory without search permission ── */
/* CosmoRT single-user: root bypasses permission checks.
   Root can chroot anywhere. Note this as expected. */

static void test_chroot04(void) {
    puts("\n[ltp/chroot04]\n");
    puts("  NOTE  single-user: root bypasses EACCES\n");

    sc2(SYS_MKDIR, (long)"/tmp/chroot04_dir", 0222);

    /* As root, chroot should succeed even without search permission */
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        long r = sc1(SYS_CHROOT, (long)"/tmp/chroot04_dir");
        sc1(SYS_EXIT, r == 0 ? 0 : 1);
    }
    check("fork", pid > 0);

    if (pid > 0) {
        int status = 0;
        sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
        int code = (status >> 8) & 0xFF;
        check_val("root chroot to 0222 dir succeeds", (long)code, 0);
    }

    sc1(SYS_RMDIR, (long)"/tmp/chroot04_dir");
}

TEST("ltp/chroot01",              test_chroot01);
TEST("ltp/chroot02",              test_chroot02);
TEST("ltp/chroot03-enametoolong", test_chroot03_enametoolong);
TEST("ltp/chroot03-enotdir",      test_chroot03_enotdir);
TEST("ltp/chroot03-enoent",       test_chroot03_enoent);
TEST("ltp/chroot03-eloop",        test_chroot03_eloop);
TEST("ltp/chroot04",              test_chroot04);
