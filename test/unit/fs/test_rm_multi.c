#include "ktest.h"

/* ── rm multiple files: unlink N files from same directory ── */

static void test_rm_multi(void) {
    puts("\n[rm-multi] create+unlink 10 files in /tmp/rmtest\n");

    /* Create directory */
    long r = sc2(SYS_MKDIR, (long)"/tmp/rmtest", 0755);
    check("mkdir /tmp/rmtest", r == 0 || r == -EEXIST);

    /* Create 10 .err files */
    const char *names[] = {
        "/tmp/rmtest/a.err",
        "/tmp/rmtest/b.err",
        "/tmp/rmtest/c.err",
        "/tmp/rmtest/d.err",
        "/tmp/rmtest/e.err",
        "/tmp/rmtest/f.err",
        "/tmp/rmtest/g.err",
        "/tmp/rmtest/h.err",
        "/tmp/rmtest/i.err",
        "/tmp/rmtest/j.err",
    };

    for (int i = 0; i < 10; i++) {
        long fd = sc3(SYS_OPEN, (long)names[i], O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            puts("  FAIL  create "); puts(names[i]);
            puts(" rc="); put_int(fd); puts("\n");
            failures++;
            return;
        }
        sc3(SYS_WRITE, fd, (long)"x", 1);
        sc1(SYS_CLOSE, fd);
    }
    pass("created 10 files");

    /* Unlink all 10 — this is what rm does */
    int ok = 1;
    for (int i = 0; i < 10; i++) {
        r = sc1(SYS_UNLINK, (long)names[i]);
        if (r != 0) {
            puts("  FAIL  unlink "); puts(names[i]);
            puts(" rc="); put_int(r); puts("\n");
            failures++;
            ok = 0;
        }
    }
    if (ok) pass("all 10 unlinks succeeded");

    /* Verify all gone */
    ok = 1;
    for (int i = 0; i < 10; i++) {
        long fd = sc3(SYS_OPEN, (long)names[i], O_RDONLY, 0);
        if (fd != -ENOENT) {
            puts("  FAIL  verify-gone "); puts(names[i]);
            puts(" rc="); put_int(fd); puts("\n");
            failures++;
            ok = 0;
            if (fd >= 0) sc1(SYS_CLOSE, fd);
        }
    }
    if (ok) pass("all 10 files gone after unlink");

    /* Cleanup dir */
    sc1(SYS_RMDIR, (long)"/tmp/rmtest");
}

TEST("rm-multi", test_rm_multi);

/* ── unlink with relative path via CWD ── */

static void test_unlink_cwd(void) {
    puts("\n[unlink-cwd] unlink relative paths after chdir\n");

    /* Create directory and files */
    long r = sc2(SYS_MKDIR, (long)"/tmp/cwdtest", 0755);
    check("mkdir /tmp/cwdtest", r == 0 || r == -EEXIST);

    const char *abs_names[] = {
        "/tmp/cwdtest/foo.txt",
        "/tmp/cwdtest/bar.txt",
        "/tmp/cwdtest/baz.txt",
    };
    for (int i = 0; i < 3; i++) {
        long fd = sc3(SYS_OPEN, (long)abs_names[i], O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            puts("  FAIL  create "); puts(abs_names[i]);
            puts(" rc="); put_int(fd); puts("\n");
            failures++;
            return;
        }
        sc1(SYS_CLOSE, fd);
    }
    pass("created 3 files");

    /* chdir to /tmp/cwdtest */
    r = sc1(SYS_CHDIR, (long)"/tmp/cwdtest");
    check_val("chdir /tmp/cwdtest", r, 0);

    /* Unlink using relative paths */
    const char *rel_names[] = { "foo.txt", "bar.txt", "baz.txt" };
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        r = sc1(SYS_UNLINK, (long)rel_names[i]);
        if (r != 0) {
            puts("  FAIL  unlink "); puts(rel_names[i]);
            puts(" rc="); put_int(r); puts("\n");
            failures++;
            ok = 0;
        }
    }
    if (ok) pass("unlink relative paths after chdir");

    /* Restore CWD */
    sc1(SYS_CHDIR, (long)"/");

    /* Cleanup */
    sc1(SYS_RMDIR, (long)"/tmp/cwdtest");
}

TEST("unlink-cwd", test_unlink_cwd);
