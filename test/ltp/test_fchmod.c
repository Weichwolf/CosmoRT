/* LTP fchmod01-06 — ported to ktest */
#include "ktest.h"

/* ── fchmod01: fchmod succeeds for various mode bits ── */

static void test_fchmod01(void) {
    puts("\n[ltp/fchmod01]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchmod01_file", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;

    int modes[] = {0, 07, 070, 0700, 0777, 02777, 04777, 06777};
    struct k_stat st;

    for (int i = 0; i < 8; i++) {
        long r = sc2(SYS_FCHMOD, fd, modes[i]);
        check("fchmod", r == 0);
        if (r != 0) continue;

        r = sc2(SYS_FSTAT, fd, (long)&st);
        check("fstat", r == 0);
        if (r == 0)
            check_val("mode bits", (long)(st.st_mode & 07777), (long)modes[i]);
    }

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fchmod01_file");
}

/* ── fchmod02: root can set sticky bit on file owned by other ── */

static void test_fchmod02(void) {
    puts("\n[ltp/fchmod02]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchmod02_file", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;

    /* Change owner to simulate "other" — we stay root */
    sc3(SYS_FCHOWN, fd, 1000, 1000);

    long r = sc2(SYS_FCHMOD, fd, 01777);
    check_val("fchmod 01777", r, 0);

    struct k_stat st;
    r = sc2(SYS_FSTAT, fd, (long)&st);
    check("fstat", r == 0);
    if (r == 0)
        check_val("sticky+perms", (long)(st.st_mode & 07777), 01777);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fchmod02_file");
}

/* ── fchmod03: fchmod sets sticky bit (owner, non-root) ── */
/* CosmoRT single-user: we are root, so just verify sticky works */

static void test_fchmod03(void) {
    puts("\n[ltp/fchmod03]\n");
    puts("  NOTE  single-user: always root\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchmod03_file", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;

    long r = sc2(SYS_FCHMOD, fd, 01777);
    check_val("fchmod 01777", r, 0);

    struct k_stat st;
    r = sc2(SYS_FSTAT, fd, (long)&st);
    check("fstat", r == 0);
    if (r == 0)
        check_val("sticky+perms", (long)(st.st_mode & 07777), 01777);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fchmod03_file");
}

/* ── fchmod04: fchmod on directory sets sticky bit ── */

static void test_fchmod04(void) {
    puts("\n[ltp/fchmod04]\n");

    sc2(SYS_MKDIR, (long)"/tmp/fchmod04_dir", 0755);

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchmod04_dir", O_RDONLY | O_DIRECTORY, 0);
    check("open dir", fd >= 0);
    if (fd < 0) { sc1(SYS_RMDIR, (long)"/tmp/fchmod04_dir"); return; }

    long r = sc2(SYS_FCHMOD, fd, 01777);
    check_val("fchmod 01777", r, 0);

    struct k_stat st;
    r = sc2(SYS_FSTAT, fd, (long)&st);
    check("fstat", r == 0);
    if (r == 0)
        check_val("sticky+perms", (long)(st.st_mode & 07777), 01777);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_RMDIR, (long)"/tmp/fchmod04_dir");
}

/* ── fchmod05: setgid cleared on dir when process gid != dir gid ── */
/* CosmoRT single-user: root can set ISGID freely.
   We test the basic dir fchmod with ISGID|ISVTX works. */

static void test_fchmod05(void) {
    puts("\n[ltp/fchmod05]\n");
    puts("  NOTE  single-user: root can set ISGID\n");

    sc2(SYS_MKDIR, (long)"/tmp/fchmod05_dir", 0755);

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchmod05_dir", O_RDONLY | O_DIRECTORY, 0);
    check("open dir", fd >= 0);
    if (fd < 0) { sc1(SYS_RMDIR, (long)"/tmp/fchmod05_dir"); return; }

    long r = sc2(SYS_FCHMOD, fd, 043777);
    check_val("fchmod 043777", r, 0);

    struct k_stat st;
    r = sc2(SYS_FSTAT, fd, (long)&st);
    check("fstat", r == 0);
    if (r == 0)
        check_val("sgid+sticky+perms", (long)(st.st_mode & 07777), 03777);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_RMDIR, (long)"/tmp/fchmod05_dir");
}

/* ── fchmod06: error cases ── */

static void test_fchmod06_ebadf(void) {
    puts("\n[ltp/fchmod06-ebadf]\n");

    /* Open and close to get a stale fd */
    long fd = sc3(SYS_OPEN, (long)"/tmp/fchmod06_file", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    long r = sc2(SYS_FCHMOD, fd, 0644);
    check_val("fchmod closed fd EBADF", r, -EBADF);

    r = sc2(SYS_FCHMOD, -1, 0644);
    check_val("fchmod(-1) EBADF", r, -EBADF);

    sc1(SYS_UNLINK, (long)"/tmp/fchmod06_file");
}

TEST("ltp/fchmod01",       test_fchmod01);
TEST("ltp/fchmod02",       test_fchmod02);
TEST("ltp/fchmod03",       test_fchmod03);
TEST("ltp/fchmod04",       test_fchmod04);
TEST("ltp/fchmod05",       test_fchmod05);
TEST("ltp/fchmod06-ebadf", test_fchmod06_ebadf);
