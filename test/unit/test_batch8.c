#include "ktest.h"

/* ── prctl tests ─────────────────────────────── */

static void test_prctl(void) {
    puts("\n[prctl]\n");

    /* PR_SET_NAME / PR_GET_NAME roundtrip */
    const char name[] = "test_thread";
    long r = sc5(SYS_PRCTL, PR_SET_NAME, (long)name, 0, 0, 0);
    check_val("prctl PR_SET_NAME", r, 0);

    char got[16] = {0};
    r = sc5(SYS_PRCTL, PR_GET_NAME, (long)got, 0, 0, 0);
    check_val("prctl PR_GET_NAME", r, 0);
    /* Compare first 11 chars */
    int match = 1;
    for (int i = 0; i < 11; i++)
        if (got[i] != name[i]) { match = 0; break; }
    check("PR_GET_NAME matches", match);

    /* PR_SET_PDEATHSIG — no-op, should return 0 */
    r = sc5(SYS_PRCTL, PR_SET_PDEATHSIG, 0, 0, 0, 0);
    check_val("prctl PR_SET_PDEATHSIG", r, 0);

    /* Invalid option → -EINVAL */
    r = sc5(SYS_PRCTL, 9999, 0, 0, 0, 0);
    check_val("prctl invalid → EINVAL", r, -EINVAL);
}

TEST("prctl", test_prctl);

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

/* ── pipe2 flags tests ───────────────────────── */

#define SYS_PIPE2   293
#define SYS_FCNTL   72

static void test_pipe2_flags(void) {
    puts("\n[pipe2 flags]\n");

    int fds[2] = {-1, -1};

    /* pipe2 with O_CLOEXEC */
    long r = sc2(SYS_PIPE2, (long)fds, O_CLOEXEC);
    check_val("pipe2 O_CLOEXEC returns 0", r, 0);
    check("pipe2 fds valid", fds[0] >= 0 && fds[1] >= 0);

    /* Check FD_CLOEXEC is set via F_GETFD */
    long fd_flags = sc3(SYS_FCNTL, fds[0], F_GETFD, 0);
    check("pipe2 read FD_CLOEXEC", fd_flags & FD_CLOEXEC);
    fd_flags = sc3(SYS_FCNTL, fds[1], F_GETFD, 0);
    check("pipe2 write FD_CLOEXEC", fd_flags & FD_CLOEXEC);

    sc1(SYS_CLOSE, fds[0]);
    sc1(SYS_CLOSE, fds[1]);

    /* pipe2 with O_NONBLOCK */
    r = sc2(SYS_PIPE2, (long)fds, O_NONBLOCK);
    check_val("pipe2 O_NONBLOCK returns 0", r, 0);

    /* Check O_NONBLOCK is set via F_GETFL */
    long fl_flags = sc3(SYS_FCNTL, fds[0], F_GETFL, 0);
    check("pipe2 read O_NONBLOCK", fl_flags & O_NONBLOCK);

    sc1(SYS_CLOSE, fds[0]);
    sc1(SYS_CLOSE, fds[1]);

    /* pipe2 with no flags — should still work */
    r = sc2(SYS_PIPE2, (long)fds, 0);
    check_val("pipe2 no flags returns 0", r, 0);
    sc1(SYS_CLOSE, fds[0]);
    sc1(SYS_CLOSE, fds[1]);
}

TEST("pipe2 flags", test_pipe2_flags);

/* ── getrandom large buffer tests ────────────── */

static void test_getrandom_large(void) {
    puts("\n[getrandom large]\n");

    /* 512 bytes — was previously capped at 256 */
    uint8_t buf[512];
    for (int i = 0; i < 512; i++) buf[i] = 0;
    long r = sc3(SYS_GETRANDOM, (long)buf, 512, 0);
    check_val("getrandom 512 returns 512", r, 512);

    /* Check second half has data (would be zero if capped at 256) */
    int nonzero_second = 0;
    for (int i = 256; i < 512; i++)
        if (buf[i]) nonzero_second++;
    check("getrandom second 256 bytes have data", nonzero_second > 0);

    /* 1024 bytes */
    uint8_t big[1024];
    for (int i = 0; i < 1024; i++) big[i] = 0;
    r = sc3(SYS_GETRANDOM, (long)big, 1024, 0);
    check_val("getrandom 1024 returns 1024", r, 1024);
}

TEST("getrandom large", test_getrandom_large);

/* ── MAP_SHARED test ─────────────────────────── */

#define MAP_SHARED_FLAG  0x01

static void test_map_shared(void) {
    puts("\n[MAP_SHARED]\n");

    /* mmap with MAP_SHARED | MAP_ANONYMOUS should succeed */
    long addr = sc6(SYS_MMAP, 0, 4096, PROT_RW,
                    MAP_SHARED_FLAG | MAP_ANONYMOUS, -1, 0);
    check("mmap MAP_SHARED succeeds", addr > 0);
    if (addr > 0) {
        volatile char *p = (volatile char *)addr;
        p[0] = 0x55;
        check_val("MAP_SHARED page writable", (long)p[0], 0x55);
        sc2(SYS_MUNMAP, addr, 4096);
    }
}

TEST("MAP_SHARED", test_map_shared);

/* ── /proc/self/exe test ─────────────────────── */

#define SYS_READLINK 89

static void test_proc_self_exe(void) {
    puts("\n[/proc/self/exe]\n");

    char buf[256] = {0};
    long r = sc3(SYS_READLINK, (long)"/proc/self/exe", (long)buf, 255);
    check("readlink /proc/self/exe > 0", r > 0);
    /* The initial ktest binary was loaded via proc_create_elf, exe_path="/init" */
    check("exe path starts with /", buf[0] == '/');
    puts("  exe_path: ");
    buf[r] = 0;
    puts(buf);
    puts("\n");
}

TEST("/proc/self/exe", test_proc_self_exe);

/* ── vfork test ──────────────────────────────── */

static void test_vfork(void) {
    puts("\n[vfork]\n");

    /* vfork should work (currently aliased to fork) */
    long pid = sc0(SYS_VFORK);
    if (pid < 0) {
        check("vfork succeeded", 0);
        return;
    }
    if (pid == 0) {
        /* Child */
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }
    /* Parent: wait for child */
    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check_val("vfork child exited 0", status, 0);
}

TEST("vfork", test_vfork);
