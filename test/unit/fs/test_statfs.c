#include "ktest.h"

/* ── statfs tests ────────────────────────────── */

#define SYS_STATFS  137
#define SYS_FSTATFS 138

struct test_statfs {
    long f_type;     long f_bsize;
    long f_blocks;   long f_bfree;   long f_bavail;
    long f_files;    long f_ffree;
    struct { int __val[2]; } f_fsid;
    long f_namelen;  long f_frsize;
    long f_flags;    long f_spare[4];
};

static void test_statfs(void) {
    puts("\n[statfs]\n");

    struct test_statfs st;

    /* statfs on root → CosmoFS */
    long r = sc2(SYS_STATFS, (long)"/", (long)&st);
    check_val("statfs / returns 0", r, 0);
    check_val("statfs / bsize=4096", st.f_bsize, 4096);
    check("statfs / blocks > 0", st.f_blocks > 0);
    check_val("statfs / namelen=255", st.f_namelen, 255);

    /* statfs on /proc → PROC_SUPER_MAGIC */
    r = sc2(SYS_STATFS, (long)"/proc", (long)&st);
    check_val("statfs /proc returns 0", r, 0);
    check_val("statfs /proc type", st.f_type, (long)0x9FA0);

    /* statfs on /tmp → TMPFS_MAGIC */
    r = sc2(SYS_STATFS, (long)"/tmp", (long)&st);
    check_val("statfs /tmp returns 0", r, 0);
    check_val("statfs /tmp type", st.f_type, (long)0x01021994);

    /* fstatfs on stdout (fd 1 = serial) */
    r = sc2(SYS_FSTATFS, 1, (long)&st);
    check_val("fstatfs fd=1 returns 0", r, 0);
    check_val("fstatfs bsize=4096", st.f_bsize, 4096);

    /* fstatfs on bad fd */
    r = sc2(SYS_FSTATFS, 200, (long)&st);
    check("fstatfs bad fd → error", r < 0);
}

TEST("statfs", test_statfs);
