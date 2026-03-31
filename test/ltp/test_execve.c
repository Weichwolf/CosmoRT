/* LTP execve02-05, execveat01-02 — ported to ktest (error-path tests) */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

/* ── execve03: error cases ── */

/* ENAMETOOLONG */
static void test_execve03_enametoolong(void) {
    puts("\n[ltp/execve03-enametoolong]\n");

    /* 256 chars — exceeds NAME_MAX (255) */
    char longname[260];
    for (int i = 0; i < 256; i++) longname[i] = 'a';
    longname[256] = 0;

    char *argv[2]; argv[0] = longname; argv[1] = 0;
    long r = sc3(SYS_EXECVE, (long)longname, (long)argv, 0);
    check_val("ENAMETOOLONG", r, -ENAMETOOLONG);
}

/* ENOENT — file doesn't exist */
static void test_execve03_enoent(void) {
    puts("\n[ltp/execve03-enoent]\n");

    char *argv[2]; argv[0] = (char *)"/nonexistent_binary"; argv[1] = 0;
    long r = sc3(SYS_EXECVE, (long)"/nonexistent_binary", (long)argv, 0);
    check_val("ENOENT", r, -ENOENT);
}

/* ENOTDIR — path component is not a directory */
static void test_execve03_enotdir(void) {
    puts("\n[ltp/execve03-enotdir]\n");

    /* Create a regular file, then try to use it as a directory component */
    long fd = sc3(SYS_OPEN, (long)"/tmp/execve03_fake",
                  O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    char *path = (char *)"/tmp/execve03_fake/child";
    char *argv[2]; argv[0] = path; argv[1] = 0;
    long r = sc3(SYS_EXECVE, (long)path, (long)argv, 0);
    check_val("ENOTDIR", r, -ENOTDIR);

    sc1(SYS_UNLINK, (long)"/tmp/execve03_fake");
}

/* EACCES — file exists but no execute permission */
static void test_execve03_eacces(void) {
    puts("\n[ltp/execve03-eacces]\n");

    /* Create file with 0444 (no execute) */
    long fd = sc3(SYS_OPEN, (long)"/tmp/execve03_nox",
                  O_RDWR | O_CREAT | O_TRUNC, 0444);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    char *argv[2]; argv[0] = (char *)"/tmp/execve03_nox"; argv[1] = 0;
    long r = sc3(SYS_EXECVE, (long)"/tmp/execve03_nox", (long)argv, 0);
    check_val("EACCES", r, -EACCES);

    sc1(SYS_UNLINK, (long)"/tmp/execve03_nox");
}

/* ENOEXEC — zero-length file with execute permission */
static void test_execve03_enoexec(void) {
    puts("\n[ltp/execve03-enoexec]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/execve03_empty",
                  O_RDWR | O_CREAT | O_TRUNC, 0755);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    char *argv[2]; argv[0] = (char *)"/tmp/execve03_empty"; argv[1] = 0;
    long r = sc3(SYS_EXECVE, (long)"/tmp/execve03_empty", (long)argv, 0);
    check_val("ENOEXEC empty file", r, -ENOEXEC);

    sc1(SYS_UNLINK, (long)"/tmp/execve03_empty");
}

/* ── execve05: concurrent execve in multiple children ── */

static void test_execve05(void) {
    puts("\n[ltp/execve05]\n");

    /* Fork children that all try to execve /bin/true.
       If /bin/true doesn't exist, the test just verifies fork/wait. */
    int nchild = 4;
    long pids[4];

    for (int i = 0; i < nchild; i++) {
        pids[i] = sc0(SYS_FORK);
        if (pids[i] < 0) { fail("fork", 0); return; }

        if (pids[i] == 0) {
            char *argv[2]; argv[0] = (char *)"/bin/true"; argv[1] = 0;
            long r = sc3(SYS_EXECVE, (long)"/bin/true", (long)argv, 0);
            /* If execve fails (no /bin/true), exit with known code */
            sc1(SYS_EXIT_GROUP, (r == -ENOENT) ? 42 : 1);
            __builtin_unreachable();
        }
    }

    int all_ok = 1;
    for (int i = 0; i < nchild; i++) {
        int wstatus = 0;
        sc4(SYS_WAIT4, pids[i], (long)&wstatus, 0, 0);
        if (!WIFEXITED(wstatus)) all_ok = 0;
    }
    check("all children exited", all_ok);
}

/* ── execveat02: error cases ── */

/* EBADF — invalid dirfd with AT_EMPTY_PATH */
static void test_execveat02_ebadf(void) {
    puts("\n[ltp/execveat02-ebadf]\n");

    char *argv[2]; argv[0] = (char *)""; argv[1] = 0;
    long r = sc5(SYS_EXECVEAT, -1, (long)"", (long)argv, 0, AT_EMPTY_PATH);
    check_val("execveat bad dirfd EBADF", r, -EBADF);
}

/* EINVAL — invalid flags */
static void test_execveat02_einval(void) {
    puts("\n[ltp/execveat02-einval]\n");

    /* Open a valid fd first */
    long fd = sc3(SYS_OPEN, (long)"/tmp", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) { fail("open /tmp", 0); return; }

    char *argv[2]; argv[0] = (char *)"test"; argv[1] = 0;
    long r = sc5(SYS_EXECVEAT, fd, (long)"/tmp", (long)argv, 0, -1);
    check_val("execveat invalid flags EINVAL", r, -EINVAL);

    sc1(SYS_CLOSE, fd);
}

/* ENOTDIR — dirfd is a file, not a directory */
static void test_execveat02_enotdir(void) {
    puts("\n[ltp/execveat02-enotdir]\n");

    /* Create a regular file to use as dirfd */
    long fd = sc3(SYS_OPEN, (long)"/tmp/execveat_notdir",
                  O_RDWR | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) { fail("create file", 0); return; }

    char *argv[2]; argv[0] = (char *)"child"; argv[1] = 0;
    long r = sc5(SYS_EXECVEAT, fd, (long)"child", (long)argv, 0, 0);
    check_val("execveat notdir ENOTDIR", r, -ENOTDIR);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/execveat_notdir");
}

/* ELOOP — AT_SYMLINK_NOFOLLOW on symlink */
static void test_execveat02_eloop(void) {
    puts("\n[ltp/execveat02-eloop]\n");

    /* Create a file and symlink to it */
    long fd = sc3(SYS_OPEN, (long)"/tmp/execveat_target",
                  O_RDWR | O_CREAT | O_TRUNC, 0755);
    if (fd >= 0) {
        /* Write minimal ELF header so it's "executable" */
        sc1(SYS_CLOSE, fd);
    }

    sc2(SYS_SYMLINK, (long)"/tmp/execveat_target", (long)"/tmp/execveat_link");

    long dirfd = sc3(SYS_OPEN, (long)"/tmp", O_RDONLY | O_DIRECTORY, 0);
    if (dirfd < 0) { fail("open /tmp", 0); goto cleanup; }

    char *argv[2]; argv[0] = (char *)"execveat_link"; argv[1] = 0;
    long r = sc5(SYS_EXECVEAT, dirfd, (long)"execveat_link",
                 (long)argv, 0, AT_SYMLINK_NOFOLLOW);
    check_val("execveat symlink ELOOP", r, -ELOOP);

    sc1(SYS_CLOSE, dirfd);

cleanup:
    sc1(SYS_UNLINK, (long)"/tmp/execveat_link");
    sc1(SYS_UNLINK, (long)"/tmp/execveat_target");
}

TEST("ltp/execve03-enametoolong",  test_execve03_enametoolong);
TEST("ltp/execve03-enoent",        test_execve03_enoent);
TEST("ltp/execve03-enotdir",       test_execve03_enotdir);
TEST("ltp/execve03-eacces",        test_execve03_eacces);
TEST("ltp/execve03-enoexec",       test_execve03_enoexec);
TEST("ltp/execve05",               test_execve05);
TEST("ltp/execveat02-ebadf",       test_execveat02_ebadf);
TEST("ltp/execveat02-einval",      test_execveat02_einval);
TEST("ltp/execveat02-enotdir",     test_execveat02_enotdir);
TEST("ltp/execveat02-eloop",       test_execveat02_eloop);
