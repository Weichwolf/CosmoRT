/* EROFS on read-only mount (Linux fs/namei.c may_open, mnt_want_write). */
#include "ktest.h"

static void test_mount_rofs(void) {
    puts("\n[mount/rofs]\n");

    /* Setup: fresh tmpfs mountpoint */
    long r = sc2(SYS_MKDIR, (long)"/tmp/rofs_mnt", 0755);
    check("mkdir /tmp/rofs_mnt", r == 0 || r == -EEXIST);

    /* Mount tmpfs read-write first so we can seed a file inside. */
    r = sc5(SYS_MOUNT, (long)"none", (long)"/tmp/rofs_mnt",
            (long)"tmpfs", 0, 0);
    check_val("mount tmpfs RW", r, 0);

    long fd = sc3(SYS_OPEN, (long)"/tmp/rofs_mnt/seed",
                  O_WRONLY | O_CREAT, 0644);
    check("create seed file", fd >= 0);
    if (fd >= 0) {
        sc3(SYS_WRITE, fd, (long)"x", 1);
        sc1(SYS_CLOSE, fd);
    }

    /* Remount read-only */
    r = sc5(SYS_MOUNT, (long)"none", (long)"/tmp/rofs_mnt",
            (long)"tmpfs", MS_REMOUNT | MS_RDONLY, 0);
    check_val("remount RO", r, 0);

    /* chmod on RO mount → EROFS */
    r = sc2(SYS_CHMOD, (long)"/tmp/rofs_mnt/seed", 0600);
    check_val("chmod EROFS", r, -EROFS);

    /* chown on RO mount → EROFS */
    r = sc3(SYS_CHOWN, (long)"/tmp/rofs_mnt/seed", 0, 0);
    check_val("chown EROFS", r, -EROFS);

    /* open for write on RO mount → EROFS */
    r = sc3(SYS_OPEN, (long)"/tmp/rofs_mnt/seed", O_WRONLY, 0);
    check_val("open O_WRONLY EROFS", r, -EROFS);

    /* O_CREAT on RO mount → EROFS */
    r = sc3(SYS_OPEN, (long)"/tmp/rofs_mnt/new", O_WRONLY | O_CREAT, 0644);
    check_val("open O_CREAT EROFS", r, -EROFS);

    /* unlink on RO mount → EROFS */
    r = sc1(SYS_UNLINK, (long)"/tmp/rofs_mnt/seed");
    check_val("unlink EROFS", r, -EROFS);

    /* mkdir on RO mount → EROFS */
    r = sc2(SYS_MKDIR, (long)"/tmp/rofs_mnt/newdir", 0755);
    check_val("mkdir EROFS", r, -EROFS);

    /* rename on RO mount → EROFS */
    r = sc2(SYS_RENAME, (long)"/tmp/rofs_mnt/seed",
            (long)"/tmp/rofs_mnt/seed2");
    check_val("rename EROFS", r, -EROFS);

    /* symlink on RO mount → EROFS */
    r = sc2(SYS_SYMLINK, (long)"/nowhere",
            (long)"/tmp/rofs_mnt/link");
    check_val("symlink EROFS", r, -EROFS);

    /* Opening existing file read-only on RO mount still works */
    fd = sc3(SYS_OPEN, (long)"/tmp/rofs_mnt/seed", O_RDONLY, 0);
    check("open O_RDONLY ok", fd >= 0);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    /* stat on RO mount works */
    struct k_stat st;
    r = sc2(SYS_STAT, (long)"/tmp/rofs_mnt/seed", (long)&st);
    check_val("stat ok on RO", r, 0);

    /* Remount RW to clean up, then unlink + rmdir */
    r = sc5(SYS_MOUNT, (long)"none", (long)"/tmp/rofs_mnt",
            (long)"tmpfs", MS_REMOUNT, 0);
    check_val("remount RW", r, 0);

    /* After RW: chmod succeeds */
    r = sc2(SYS_CHMOD, (long)"/tmp/rofs_mnt/seed", 0644);
    check_val("chmod after RW", r, 0);
}

TEST("mount/rofs", test_mount_rofs);
