#include "ktest.h"

static void test_dirfd(void) {
    puts("\n[dirfd]\n");

    /* openat(AT_FDCWD, absolute) → success */
    long fd = sc4(SYS_OPENAT, AT_FDCWD, (long)"/proc/dmesg", O_RDONLY, 0);
    check_ge("openat AT_FDCWD /proc/dmesg", fd, 0);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    /* faccessat(AT_FDCWD, F_OK) → 0 */
    long r = sc4(SYS_FACCESSAT, AT_FDCWD, (long)"/proc/dmesg", F_OK, 0);
    check_val("faccessat AT_FDCWD F_OK /proc/dmesg", r, 0);

    /* faccessat(AT_FDCWD, R_OK) → 0 (single-user, exists = permitted) */
    r = sc4(SYS_FACCESSAT, AT_FDCWD, (long)"/proc/dmesg", R_OK, 0);
    check_val("faccessat AT_FDCWD R_OK /proc/dmesg", r, 0);

    /* faccessat(AT_FDCWD, F_OK) nonexistent → -ENOENT */
    r = sc4(SYS_FACCESSAT, AT_FDCWD, (long)"/nonexistent", F_OK, 0);
    check_val("faccessat AT_FDCWD /nonexistent → -ENOENT", r, -ENOENT);

    /* access() with R_OK|W_OK on existing file */
    r = sc2(SYS_ACCESS, (long)"/dev/null", R_OK | W_OK);
    check_val("access /dev/null R_OK|W_OK", r, 0);

    /* openat with file fd (not dir) + relative path → -ENOTDIR */
    fd = sc4(SYS_OPENAT, AT_FDCWD, (long)"/proc/dmesg", O_RDONLY, 0);
    if (fd >= 0) {
        long r2 = sc4(SYS_OPENAT, (long)fd, (long)"relative", O_RDONLY, 0);
        check_val("openat file-dirfd relative → -ENOTDIR", r2, -ENOTDIR);
        sc1(SYS_CLOSE, fd);
    }

    /* openat with real dir fd + relative path → resolves correctly */
    fd = sc4(SYS_OPENAT, AT_FDCWD, (long)"/tmp", O_RDONLY | O_DIRECTORY, 0);
    check_ge("open /tmp O_DIRECTORY", fd, 0);
    if (fd >= 0) {
        /* Create a file via the dir fd */
        long fd3 = sc4(SYS_OPENAT, (long)fd, (long)"_dirfd_test",
                       O_CREAT | O_WRONLY, 0644);
        check_ge("openat dirfd create _dirfd_test", fd3, 0);
        if (fd3 >= 0) sc1(SYS_CLOSE, fd3);
        /* Open it back via dirfd */
        long fd4 = sc4(SYS_OPENAT, (long)fd, (long)"_dirfd_test",
                       O_RDONLY, 0);
        check_ge("openat dirfd read _dirfd_test", fd4, 0);
        if (fd4 >= 0) sc1(SYS_CLOSE, fd4);
        /* Cleanup */
        sc3(SYS_UNLINKAT, (long)fd, (long)"_dirfd_test", 0);
        sc1(SYS_CLOSE, fd);
    }

    /* renameat2 with RENAME_NOREPLACE: target exists → -EEXIST */
    /* Create two files, try noreplace rename */
    long fd1 = sc4(SYS_OPENAT, AT_FDCWD, (long)"/tmp/_rn_src", O_CREAT | O_WRONLY, 0644);
    long fd2 = sc4(SYS_OPENAT, AT_FDCWD, (long)"/tmp/_rn_dst", O_CREAT | O_WRONLY, 0644);
    if (fd1 >= 0) sc1(SYS_CLOSE, fd1);
    if (fd2 >= 0) sc1(SYS_CLOSE, fd2);
    r = sc5(SYS_RENAMEAT2, AT_FDCWD, (long)"/tmp/_rn_src", AT_FDCWD,
            (long)"/tmp/_rn_dst", RENAME_NOREPLACE);
    check_val("renameat2 NOREPLACE exists → -EEXIST", r, -EEXIST);
    /* Cleanup */
    sc3(SYS_UNLINKAT, AT_FDCWD, (long)"/tmp/_rn_src", 0);
    sc3(SYS_UNLINKAT, AT_FDCWD, (long)"/tmp/_rn_dst", 0);

    /* renameat2 with RENAME_EXCHANGE → -EINVAL (not implemented) */
    r = sc5(SYS_RENAMEAT2, AT_FDCWD, (long)"/tmp/_a", AT_FDCWD,
            (long)"/tmp/_b", RENAME_EXCHANGE);
    check_val("renameat2 EXCHANGE → -EINVAL", r, -EINVAL);

    /* renameat2 with unknown flag → -EINVAL */
    r = sc5(SYS_RENAMEAT2, AT_FDCWD, (long)"/tmp/_a", AT_FDCWD,
            (long)"/tmp/_b", 0x80);
    check_val("renameat2 bad flags → -EINVAL", r, -EINVAL);

    /* linkat with AT_EMPTY_PATH → -ENOSYS */
    r = sc5(SYS_LINKAT, AT_FDCWD, (long)"/tmp/_x", AT_FDCWD,
            (long)"/tmp/_y", AT_EMPTY_PATH);
    check_val("linkat AT_EMPTY_PATH → -ENOSYS", r, -ENOSYS);

    /* linkat with invalid flags → -EINVAL */
    r = sc5(SYS_LINKAT, AT_FDCWD, (long)"/tmp/_x", AT_FDCWD,
            (long)"/tmp/_y", 0x8000);
    check_val("linkat bad flags → -EINVAL", r, -EINVAL);
}

TEST("dirfd", test_dirfd);
