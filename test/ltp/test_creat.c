/* LTP creat01, creat03, creat04, creat06, creat07, creat08 — ported to ktest */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

/* ── creat01: basic creat creates file with correct mode ── */

static void test_creat01(void) {
    puts("\n[ltp/creat01]\n");

    sc1(SYS_UNLINK, (long)"/tmp/creat01");

    long fd = sc2(SYS_CREAT, (long)"/tmp/creat01", 0644);
    check("creat", fd >= 0);
    if (fd < 0) return;

    /* Verify file exists and has correct mode */
    struct k_stat st;
    long r = sc2(SYS_FSTAT, fd, (long)&st);
    check_val("fstat", r, 0);
    check_val("mode 0644", (long)(st.st_mode & 0777), 0644);

    /* File should be writable */
    r = sc3(SYS_WRITE, fd, (long)"hello", 5);
    check_val("write 5", r, 5);

    sc1(SYS_CLOSE, fd);

    /* Verify via stat that file has size 5 */
    r = sc2(SYS_STAT, (long)"/tmp/creat01", (long)&st);
    check_val("stat", r, 0);
    check_val("size 5", (long)st.st_size, 5);

    sc1(SYS_UNLINK, (long)"/tmp/creat01");
}

/* ── creat03: creat truncates existing file ── */

static void test_creat03(void) {
    puts("\n[ltp/creat03]\n");

    /* Create file with content */
    long fd = sc3(SYS_OPEN, (long)"/tmp/creat03", O_RDWR | O_CREAT, 0644);
    check("initial open", fd >= 0);
    if (fd < 0) return;
    sc3(SYS_WRITE, fd, (long)"abcdefghij", 10);
    sc1(SYS_CLOSE, fd);

    /* creat should truncate */
    fd = sc2(SYS_CREAT, (long)"/tmp/creat03", 0644);
    check("creat truncate", fd >= 0);
    if (fd < 0) return;

    struct k_stat st;
    long r = sc2(SYS_FSTAT, fd, (long)&st);
    check_val("fstat", r, 0);
    check_val("size 0 after truncate", (long)st.st_size, 0);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/creat03");
}

/* ── creat04: creat in non-existent directory → ENOENT ── */

static void test_creat04(void) {
    puts("\n[ltp/creat04]\n");

    long fd = sc2(SYS_CREAT, (long)"/tmp/no_such_dir_creat04/file", 0644);
    check_val("creat ENOENT", fd, -ENOENT);
}

/* ── creat06: creat with different modes ── */

static void test_creat06(void) {
    puts("\n[ltp/creat06]\n");

    /* mode 0444 (read-only) */
    sc1(SYS_UNLINK, (long)"/tmp/creat06a");
    long fd = sc2(SYS_CREAT, (long)"/tmp/creat06a", 0444);
    check("creat 0444", fd >= 0);
    if (fd >= 0) {
        struct k_stat st;
        sc2(SYS_FSTAT, fd, (long)&st);
        check_val("mode 0444", (long)(st.st_mode & 0777), 0444);
        sc1(SYS_CLOSE, fd);
    }
    sc1(SYS_UNLINK, (long)"/tmp/creat06a");

    /* mode 0700 */
    sc1(SYS_UNLINK, (long)"/tmp/creat06b");
    fd = sc2(SYS_CREAT, (long)"/tmp/creat06b", 0700);
    check("creat 0700", fd >= 0);
    if (fd >= 0) {
        struct k_stat st;
        sc2(SYS_FSTAT, fd, (long)&st);
        check_val("mode 0700", (long)(st.st_mode & 0777), 0700);
        sc1(SYS_CLOSE, fd);
    }
    sc1(SYS_UNLINK, (long)"/tmp/creat06b");

    /* mode 0000 */
    sc1(SYS_UNLINK, (long)"/tmp/creat06c");
    fd = sc2(SYS_CREAT, (long)"/tmp/creat06c", 0000);
    check("creat 0000", fd >= 0);
    if (fd >= 0) {
        struct k_stat st;
        sc2(SYS_FSTAT, fd, (long)&st);
        check_val("mode 0000", (long)(st.st_mode & 0777), 0000);
        sc1(SYS_CLOSE, fd);
    }
    sc1(SYS_UNLINK, (long)"/tmp/creat06c");
}

/* ── creat07: creat returns write-only fd (O_WRONLY) ── */

static void test_creat07(void) {
    puts("\n[ltp/creat07]\n");

    sc1(SYS_UNLINK, (long)"/tmp/creat07");

    long fd = sc2(SYS_CREAT, (long)"/tmp/creat07", 0644);
    check("creat", fd >= 0);
    if (fd < 0) return;

    /* creat = open(O_CREAT|O_WRONLY|O_TRUNC), so read should fail with EBADF */
    char buf[8];
    long r = sc3(SYS_READ, fd, (long)buf, 8);
    check_val("read on creat fd EBADF", r, -EBADF);

    /* Write should succeed */
    r = sc3(SYS_WRITE, fd, (long)"test", 4);
    check_val("write succeeds", r, 4);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/creat07");
}

/* ── creat08: creat with name too long → ENAMETOOLONG ── */

static void test_creat08(void) {
    puts("\n[ltp/creat08]\n");

    /* Build a path component longer than 255 chars */
    char longname[300];
    int i;
    longname[0] = '/'; longname[1] = 't'; longname[2] = 'm';
    longname[3] = 'p'; longname[4] = '/';
    for (i = 5; i < 5 + 256; i++)
        longname[i] = 'a';
    longname[i] = '\0';

    long fd = sc2(SYS_CREAT, (long)longname, 0644);
    check_val("creat ENAMETOOLONG", fd, -ENAMETOOLONG);
}

TEST("ltp/creat01", test_creat01);
TEST("ltp/creat03", test_creat03);
TEST("ltp/creat04", test_creat04);
TEST("ltp/creat06", test_creat06);
TEST("ltp/creat07", test_creat07);
TEST("ltp/creat08", test_creat08);
