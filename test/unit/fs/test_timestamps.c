/* test: Timestamps — ext4 and ramfs mtime */
#include "ktest.h"

/* 2020-01-01 00:00:00 UTC = 1577836800 */
#define TS_2020 1577836800L

/* Create file on ext4 (root fs), stat → mtime > 2020 */
static void test_ext4_timestamp(void) {
    puts("\n[timestamps: ext4 create]\n");

    long fd = sc3(SYS_OPEN, (long)"/_ts_test", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    check("create /_ts_test", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    struct k_stat st;
    long r = sc2(SYS_STAT, (long)"/_ts_test", (long)&st);
    check_val("stat returns 0", r, 0);
    puts("  mtime_s="); put_int(st.st_mtime_sec); puts("\n");
    check("mtime > 2020", st.st_mtime_sec > TS_2020);

    sc3(SYS_UNLINKAT, AT_FDCWD, (long)"/_ts_test", 0);
}

/* Write to ext4 file, stat → mtime is current (not 0) */
static void test_ext4_write_updates_mtime(void) {
    puts("\n[timestamps: ext4 write mtime]\n");

    long fd = sc3(SYS_OPEN, (long)"/_ts_wr", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    /* Write to file */
    fd = sc3(SYS_OPEN, (long)"/_ts_wr", O_WRONLY | O_APPEND, 0);
    check("open for write", fd >= 0);
    if (fd >= 0) {
        sc3(SYS_WRITE, fd, (long)"data", 4);
        sc1(SYS_CLOSE, fd);
    }

    struct k_stat st;
    sc2(SYS_STAT, (long)"/_ts_wr", (long)&st);
    puts("  mtime_s="); put_int(st.st_mtime_sec); puts("\n");

    /* After write, mtime must be recent (> 2020) */
    check("mtime after write > 2020", st.st_mtime_sec > TS_2020);
    /* Size should reflect the write */
    check_val("size = 4", st.st_size, 4);

    sc3(SYS_UNLINKAT, AT_FDCWD, (long)"/_ts_wr", 0);
}

/* Create file in /tmp (ramfs), stat → mtime > 2020 */
static void test_ramfs_timestamp(void) {
    puts("\n[timestamps: ramfs create]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/_ts_ram", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    check("create /tmp/_ts_ram", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    struct k_stat st;
    long r = sc2(SYS_STAT, (long)"/tmp/_ts_ram", (long)&st);
    check_val("stat returns 0", r, 0);
    puts("  mtime_s="); put_int(st.st_mtime_sec); puts("\n");
    check("mtime > 2020", st.st_mtime_sec > TS_2020);

    sc3(SYS_UNLINKAT, AT_FDCWD, (long)"/tmp/_ts_ram", 0);
}

/* Write to ramfs file, stat → mtime reflects write */
static void test_ramfs_write_mtime(void) {
    puts("\n[timestamps: ramfs write mtime]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/_ts_ram2", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    check("create", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    fd = sc3(SYS_OPEN, (long)"/tmp/_ts_ram2", O_WRONLY | O_APPEND, 0);
    check("open for write", fd >= 0);
    if (fd >= 0) {
        sc3(SYS_WRITE, fd, (long)"data", 4);
        sc1(SYS_CLOSE, fd);
    }

    struct k_stat st;
    sc2(SYS_STAT, (long)"/tmp/_ts_ram2", (long)&st);
    puts("  mtime_s="); put_int(st.st_mtime_sec); puts("\n");

    /* After write, mtime must be recent */
    check("mtime after write > 2020", st.st_mtime_sec > TS_2020);
    check_val("size = 4", st.st_size, 4);

    sc3(SYS_UNLINKAT, AT_FDCWD, (long)"/tmp/_ts_ram2", 0);
}

TEST("ts/ext4-create", test_ext4_timestamp);
TEST("ts/ext4-write-mtime", test_ext4_write_updates_mtime);
TEST("ts/ramfs-create", test_ramfs_timestamp);
TEST("ts/ramfs-write-mtime", test_ramfs_write_mtime);
