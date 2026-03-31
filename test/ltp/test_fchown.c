/* LTP fchown01-05 — ported to ktest */
#include "ktest.h"

/* ── fchown01: basic fchown succeeds ── */

static void test_fchown01(void) {
    puts("\n[ltp/fchown01]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchown01_file", O_RDWR | O_CREAT, 0700);
    check("create", fd >= 0);
    if (fd < 0) return;

    long r = sc3(SYS_FCHOWN, fd, 0, 0);
    check_val("fchown(0,0)", r, 0);

    struct k_stat st;
    r = sc2(SYS_FSTAT, fd, (long)&st);
    check("fstat", r == 0);
    if (r == 0) {
        check_val("uid", (long)st.st_uid, 0);
        check_val("gid", (long)st.st_gid, 0);
    }

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fchown01_file");
}

/* ── fchown02: fchown clears suid/sgid on executable, preserves on non-exec ── */

static void test_fchown02_clears(void) {
    puts("\n[ltp/fchown02-clears]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchown02_f1", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;

    /* Set ISUID|ISGID|IRWXU|IRWXG */
    sc2(SYS_FCHMOD, fd, 06770);

    long r = sc3(SYS_FCHOWN, fd, 0, 0);
    check_val("fchown", r, 0);

    struct k_stat st;
    r = sc2(SYS_FSTAT, fd, (long)&st);
    check("fstat", r == 0);
    if (r == 0) {
        check_val("suid/sgid cleared", (long)(st.st_mode & 06000), 0);
        check("rwx preserved", (st.st_mode & 0770) == 0770);
    }

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fchown02_f1");
}

static void test_fchown02_preserves(void) {
    puts("\n[ltp/fchown02-preserves]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchown02_f2", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;

    /* ISGID|IRWXU only (no group-execute) */
    sc2(SYS_FCHMOD, fd, 02700);

    long r = sc3(SYS_FCHOWN, fd, 0, 0);
    check_val("fchown", r, 0);

    struct k_stat st;
    r = sc2(SYS_FSTAT, fd, (long)&st);
    check("fstat", r == 0);
    if (r == 0)
        check_val("sgid preserved", (long)(st.st_mode & 02000), 02000);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fchown02_f2");
}

/* ── fchown03: fchown clears suid/sgid on gid change ── */
/* Original runs as non-root owner. CosmoRT single-user: always root. */

static void test_fchown03(void) {
    puts("\n[ltp/fchown03]\n");
    puts("  NOTE  single-user: always root, testing sgid clear on gid change\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchown03_file", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;

    /* Set ISUID|ISGID|IRWXU|IRWXG */
    sc2(SYS_FCHMOD, fd, 06770);

    /* Change only gid */
    long r = sc3(SYS_FCHOWN, fd, (long)-1, 0);
    check_val("fchown(-1, 0)", r, 0);

    struct k_stat st;
    r = sc2(SYS_FSTAT, fd, (long)&st);
    check("fstat", r == 0);
    if (r == 0) {
        check_val("uid unchanged", (long)st.st_uid, 0);
        check_val("gid set", (long)st.st_gid, 0);
    }

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fchown03_file");
}

/* ── fchown04: error cases ── */

static void test_fchown04_ebadf(void) {
    puts("\n[ltp/fchown04-ebadf]\n");

    long r = sc3(SYS_FCHOWN, -1, 0, 0);
    check_val("fchown(-1) EBADF", r, -EBADF);

    /* Closed fd */
    long fd = sc3(SYS_OPEN, (long)"/tmp/fchown04_file", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    r = sc3(SYS_FCHOWN, fd, 0, 0);
    check_val("fchown closed fd EBADF", r, -EBADF);

    sc1(SYS_UNLINK, (long)"/tmp/fchown04_file");
}

/* ── fchown05: fchown to arbitrary uid/gid as root ── */

static void test_fchown05(void) {
    puts("\n[ltp/fchown05]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fchown05_file", O_RDWR | O_CREAT, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;

    struct k_stat st;

    /* change both */
    long r = sc3(SYS_FCHOWN, fd, 700, 701);
    check_val("fchown(700,701)", r, 0);
    r = sc2(SYS_FSTAT, fd, (long)&st);
    if (r == 0) { check_val("uid", (long)st.st_uid, 700); check_val("gid", (long)st.st_gid, 701); }

    /* change owner only */
    r = sc3(SYS_FCHOWN, fd, 702, (long)-1);
    check_val("fchown(702,-1)", r, 0);
    r = sc2(SYS_FSTAT, fd, (long)&st);
    if (r == 0) { check_val("uid", (long)st.st_uid, 702); check_val("gid unchanged", (long)st.st_gid, 701); }

    /* change group only */
    r = sc3(SYS_FCHOWN, fd, (long)-1, 704);
    check_val("fchown(-1,704)", r, 0);
    r = sc2(SYS_FSTAT, fd, (long)&st);
    if (r == 0) { check_val("uid unchanged", (long)st.st_uid, 702); check_val("gid", (long)st.st_gid, 704); }

    /* no change */
    r = sc3(SYS_FCHOWN, fd, (long)-1, (long)-1);
    check_val("fchown(-1,-1)", r, 0);
    r = sc2(SYS_FSTAT, fd, (long)&st);
    if (r == 0) { check_val("uid nop", (long)st.st_uid, 702); check_val("gid nop", (long)st.st_gid, 704); }

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fchown05_file");
}

TEST("ltp/fchown01",           test_fchown01);
TEST("ltp/fchown02-clears",    test_fchown02_clears);
TEST("ltp/fchown02-preserves", test_fchown02_preserves);
TEST("ltp/fchown03",           test_fchown03);
TEST("ltp/fchown04-ebadf",     test_fchown04_ebadf);
TEST("ltp/fchown05",           test_fchown05);
