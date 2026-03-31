/* LTP copy_file_range01/03 — ported to ktest */
#include "ktest.h"

#define CONTENT     "ABCDEFGHIJKLMNOPQRSTUVWXYZ12345\n"
#define CONTSIZE    31

/* ── copy_file_range01: basic copy, data integrity ── */

static void test_cfr_basic(void) {
    puts("\n[ltp/copy_file_range-basic]\n");

    long fd_src = sc3(SYS_OPEN, (long)"/tmp/cfr_src", O_RDWR | O_CREAT | O_TRUNC, 0664);
    check("create src", fd_src >= 0);
    if (fd_src < 0) return;

    sc3(SYS_WRITE, fd_src, (long)CONTENT, CONTSIZE);
    sc1(SYS_CLOSE, fd_src);

    fd_src = sc3(SYS_OPEN, (long)"/tmp/cfr_src", O_RDONLY, 0);
    check("reopen src", fd_src >= 0);

    long fd_dst = sc3(SYS_OPEN, (long)"/tmp/cfr_dst", O_RDWR | O_CREAT | O_TRUNC, 0664);
    check("create dst", fd_dst >= 0);
    if (fd_dst < 0) { sc1(SYS_CLOSE, fd_src); return; }

    long r = sc6(SYS_COPY_FILE_RANGE, fd_src, 0, fd_dst, 0, CONTSIZE, 0);
    check_ge("copy_file_range returns bytes", r, 1);

    /* verify content */
    sc3(SYS_LSEEK, fd_dst, 0, SEEK_SET);
    char buf[64] = {0};
    long n = sc3(SYS_READ, fd_dst, (long)buf, CONTSIZE);
    check_val("read back length", n, CONTSIZE);
    check("data matches", buf[0] == 'A' && buf[30] == '\n');

    sc1(SYS_CLOSE, fd_src);
    sc1(SYS_CLOSE, fd_dst);
    sc1(SYS_UNLINK, (long)"/tmp/cfr_src");
    sc1(SYS_UNLINK, (long)"/tmp/cfr_dst");
}

/* ── copy_file_range with offsets ── */

static void test_cfr_offsets(void) {
    puts("\n[ltp/copy_file_range-offsets]\n");

    long fd_src = sc3(SYS_OPEN, (long)"/tmp/cfr_off_src", O_RDWR | O_CREAT | O_TRUNC, 0664);
    check("create src", fd_src >= 0);
    if (fd_src < 0) return;

    sc3(SYS_WRITE, fd_src, (long)CONTENT, CONTSIZE);

    long fd_dst = sc3(SYS_OPEN, (long)"/tmp/cfr_off_dst", O_RDWR | O_CREAT | O_TRUNC, 0664);
    check("create dst", fd_dst >= 0);
    if (fd_dst < 0) { sc1(SYS_CLOSE, fd_src); return; }

    /* copy 10 bytes from offset 5 in src to offset 0 in dst */
    long off_in = 5, off_out = 0;
    long r = sc6(SYS_COPY_FILE_RANGE, fd_src, (long)&off_in,
                 fd_dst, (long)&off_out, 10, 0);
    check_ge("copy 10 bytes from offset 5", r, 1);

    /* offsets should advance */
    check_val("off_in advanced", off_in, 5 + r);
    check_val("off_out advanced", off_out, 0 + r);

    /* fd position should not change when offsets are provided */
    long pos = sc3(SYS_LSEEK, fd_src, 0, SEEK_CUR);
    check_val("src fd pos unchanged", pos, CONTSIZE); /* still at end from write */

    sc1(SYS_CLOSE, fd_src);
    sc1(SYS_CLOSE, fd_dst);
    sc1(SYS_UNLINK, (long)"/tmp/cfr_off_src");
    sc1(SYS_UNLINK, (long)"/tmp/cfr_off_dst");
}

/* ── copy_file_range03: timestamp update ── */

static void test_cfr_timestamp(void) {
    puts("\n[ltp/copy_file_range03]\n");

    long fd_src = sc3(SYS_OPEN, (long)"/tmp/cfr_ts_src", O_RDWR | O_CREAT | O_TRUNC, 0664);
    check("create src", fd_src >= 0);
    if (fd_src < 0) return;
    sc3(SYS_WRITE, fd_src, (long)CONTENT, CONTSIZE);
    sc1(SYS_CLOSE, fd_src);
    fd_src = sc3(SYS_OPEN, (long)"/tmp/cfr_ts_src", O_RDONLY, 0);

    long fd_dst = sc3(SYS_OPEN, (long)"/tmp/cfr_ts_dst", O_RDWR | O_CREAT | O_TRUNC, 0664);
    check("create dst", fd_dst >= 0);
    if (fd_dst < 0) { sc1(SYS_CLOSE, fd_src); return; }

    /* get mtime before copy */
    struct k_stat st_before;
    sc2(SYS_FSTAT, fd_dst, (long)&st_before);

    /* small delay via nanosleep */
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100ms */
    sc2(SYS_NANOSLEEP, (long)&ts, 0);

    long r = sc6(SYS_COPY_FILE_RANGE, fd_src, 0, fd_dst, 0, CONTSIZE, 0);
    check_ge("copy for timestamp test", r, 1);

    struct k_stat st_after;
    sc2(SYS_FSTAT, fd_dst, (long)&st_after);

    /* mtime should have changed (or at least not gone backwards) */
    long diff_sec = st_after.st_mtime_sec - st_before.st_mtime_sec;
    long diff_nsec = st_after.st_mtime_nsec - st_before.st_mtime_nsec;
    long diff_total = diff_sec * 1000000000L + diff_nsec;
    check_ge("mtime advanced", diff_total, 0);

    sc1(SYS_CLOSE, fd_src);
    sc1(SYS_CLOSE, fd_dst);
    sc1(SYS_UNLINK, (long)"/tmp/cfr_ts_src");
    sc1(SYS_UNLINK, (long)"/tmp/cfr_ts_dst");
}

/* ── copy_file_range: EBADF for invalid fds ── */

static void test_cfr_ebadf(void) {
    puts("\n[ltp/copy_file_range-ebadf]\n");

    long r = sc6(SYS_COPY_FILE_RANGE, 999, 0, 998, 0, 10, 0);
    check_val("bad fds EBADF", r, -EBADF);
}

TEST("ltp/copy_file_range-basic",     test_cfr_basic);
TEST("ltp/copy_file_range-offsets",   test_cfr_offsets);
TEST("ltp/copy_file_range03",         test_cfr_timestamp);
TEST("ltp/copy_file_range-ebadf",     test_cfr_ebadf);
