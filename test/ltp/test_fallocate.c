/* LTP fallocate01-03 — ported to ktest */
#include "ktest.h"

#define FALLOC_FL_KEEP_SIZE 0x01

/* ── fallocate01: basic fallocate — default mode and KEEP_SIZE ── */

static void test_fallocate01(void) {
    puts("\n[ltp/fallocate01]\n");

    /* Default mode: fallocate extends visible file size */
    long fd = sc3(SYS_OPEN, (long)"/tmp/falloc01a", O_RDWR | O_CREAT | O_TRUNC, 0700);
    check("open mode1", fd >= 0);
    if (fd < 0) return;

    /* Write some data first */
    sc3(SYS_WRITE, fd, (long)"ABCDEFGHIJKLMNOP", 16);

    /* fallocate(fd, 0, 16, 4096) — extends from offset 16 by 4096 */
    long r = sc4(SYS_FALLOCATE, fd, 0, 16, 4096);
    if (r == -ENOSYS || r == -95 /* EOPNOTSUPP */) {
        puts("  SKIP  fallocate not supported\n");
        sc1(SYS_CLOSE, fd);
        sc1(SYS_UNLINK, (long)"/tmp/falloc01a");
        return;
    }
    check_val("fallocate default", r, 0);

    struct k_stat st;
    sc2(SYS_FSTAT, fd, (long)&st);
    check("size grew", st.st_size >= 16 + 4096);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/falloc01a");

    /* FALLOC_FL_KEEP_SIZE: size stays the same */
    fd = sc3(SYS_OPEN, (long)"/tmp/falloc01b", O_RDWR | O_CREAT | O_TRUNC, 0700);
    check("open mode2", fd >= 0);
    if (fd < 0) return;

    sc3(SYS_WRITE, fd, (long)"ABCDEFGHIJKLMNOP", 16);
    sc2(SYS_FSTAT, fd, (long)&st);
    long orig_size = st.st_size;

    r = sc4(SYS_FALLOCATE, fd, FALLOC_FL_KEEP_SIZE, 16, 4096);
    check_val("fallocate KEEP_SIZE", r, 0);

    sc2(SYS_FSTAT, fd, (long)&st);
    check_val("size unchanged", st.st_size, orig_size);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/falloc01b");
}

/* ── fallocate02: error conditions ── */

static void test_fallocate02(void) {
    puts("\n[ltp/fallocate02]\n");

    /* EBADF: read-only fd */
    long fdr = sc3(SYS_OPEN, (long)"/tmp/falloc02r", O_RDONLY | O_CREAT, 0444);
    check("open rdonly", fdr >= 0);
    if (fdr >= 0) {
        long r = sc4(SYS_FALLOCATE, fdr, 0, 0, 1024);
        check_val("EBADF rdonly", r, -EBADF);
        sc1(SYS_CLOSE, fdr);
        sc1(SYS_UNLINK, (long)"/tmp/falloc02r");
    }

    long fdw = sc3(SYS_OPEN, (long)"/tmp/falloc02w", O_RDWR | O_CREAT, 0666);
    check("open rdwr", fdw >= 0);
    if (fdw < 0) return;

    /* EINVAL: negative offset */
    long r = sc4(SYS_FALLOCATE, fdw, 0, -1, 1024);
    check_val("negative offset EINVAL", r, -EINVAL);

    /* EINVAL: negative length */
    r = sc4(SYS_FALLOCATE, fdw, 0, 0, -1);
    check_val("negative length EINVAL", r, -EINVAL);

    /* EINVAL: zero length */
    r = sc4(SYS_FALLOCATE, fdw, 0, 0, 0);
    check_val("zero length EINVAL", r, -EINVAL);

    sc1(SYS_CLOSE, fdw);
    sc1(SYS_UNLINK, (long)"/tmp/falloc02w");
}

/* ── fallocate03: sparse file with holes ── */

static void test_fallocate03(void) {
    puts("\n[ltp/fallocate03]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/falloc03", O_RDWR | O_CREAT | O_TRUNC, 0700);
    check("open", fd >= 0);
    if (fd < 0) return;

    /* Write 4096 bytes, skip 4096 (hole), write 4096 */
    char buf[128];
    for (int i = 0; i < 128; i++) buf[i] = 'A' + (char)(i % 26);

    for (int i = 0; i < 32; i++)
        sc3(SYS_WRITE, fd, (long)buf, 128); /* 4096 bytes */

    sc3(SYS_LSEEK, fd, 8192, SEEK_SET);

    for (int i = 0; i < 32; i++)
        sc3(SYS_WRITE, fd, (long)buf, 128); /* 4096 more */

    /* fallocate into the hole area */
    long r = sc4(SYS_FALLOCATE, fd, 0, 4096, 4096);
    if (r == -ENOSYS || r == -95) {
        puts("  SKIP  fallocate not supported\n");
        sc1(SYS_CLOSE, fd);
        sc1(SYS_UNLINK, (long)"/tmp/falloc03");
        return;
    }
    check_val("fallocate hole", r, 0);

    struct k_stat st;
    sc2(SYS_FSTAT, fd, (long)&st);
    check("size >= 12288", st.st_size >= 12288);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/falloc03");
}

TEST("ltp/fallocate01", test_fallocate01);
TEST("ltp/fallocate02", test_fallocate02);
TEST("ltp/fallocate03", test_fallocate03);
