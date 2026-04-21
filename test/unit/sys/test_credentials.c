/* POSIX credentials: ruid/euid/suid + groups[] + setuid/seteuid/setreuid/setresuid */
#include "ktest.h"

#define NOBODY 65534U

/* fork() a child, run child_fn, wait for exit and return status. */
static long credtest_fork(void (*child_fn)(void)) {
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        child_fn();
        sc1(SYS_EXIT, 0);
        for (;;) {}
    }
    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    return status;
}

/* ── setuid as root: sets r/e/suid to same value ── */

static void child_setuid_as_root(void) {
    long r = sc1(SYS_SETUID, 500);
    check_val("setuid(500) as root", r, 0);
    check_val("getuid=500", sc0(SYS_GETUID), 500);
    check_val("geteuid=500", sc0(SYS_GETEUID), 500);
    uint32_t ruid=0, euid=0, suid=0;
    sc3(SYS_GETRESUID, (long)&ruid, (long)&euid, (long)&suid);
    check_val("ruid=500", (long)ruid, 500);
    check_val("euid=500", (long)euid, 500);
    check_val("suid=500", (long)suid, 500);
}

static void test_setuid_as_root(void) {
    puts("\n[cred/setuid_as_root]\n");
    credtest_fork(child_setuid_as_root);
}

/* ── seteuid(nobody) drops euid only; ruid/suid unchanged ── */

static void child_seteuid_nobody(void) {
    long r = sc2(SYS_SETREUID, (long)-1, NOBODY);
    check_val("seteuid(nobody)", r, 0);
    check_val("getuid=0", sc0(SYS_GETUID), 0);
    check_val("geteuid=nobody", sc0(SYS_GETEUID), NOBODY);
    /* Unprivileged now, but can go back via suid=0 */
    r = sc1(SYS_SETUID, 0);
    check_val("setuid(0) returns 0 via suid=0", r, 0);
    check_val("geteuid=0 after re-raise", sc0(SYS_GETEUID), 0);
}

static void test_seteuid_nobody(void) {
    puts("\n[cred/seteuid_nobody]\n");
    credtest_fork(child_seteuid_nobody);
}

/* ── unprivileged setuid to foreign uid fails EPERM ── */

static void child_setuid_eperm(void) {
    sc1(SYS_SETUID, NOBODY); /* all three -> nobody */
    long r = sc1(SYS_SETUID, 1234);
    check_val("setuid(1234) EPERM when unpriv", r, -EPERM);
}

static void test_setuid_eperm(void) {
    puts("\n[cred/setuid_eperm]\n");
    credtest_fork(child_setuid_eperm);
}

/* ── setresuid: full triple update, saved updated ── */

static void child_setresuid(void) {
    long r = sc3(SYS_SETRESUID, 100, 200, 300);
    check_val("setresuid(100,200,300) as root", r, 0);
    uint32_t ruid=0, euid=0, suid=0;
    sc3(SYS_GETRESUID, (long)&ruid, (long)&euid, (long)&suid);
    check_val("ruid=100", (long)ruid, 100);
    check_val("euid=200", (long)euid, 200);
    check_val("suid=300", (long)suid, 300);
    r = sc3(SYS_SETRESUID, -1, 100, -1);
    check_val("seteuid=100 via setresuid", r, 0);
    r = sc3(SYS_SETRESUID, -1, 999, -1);
    check_val("seteuid=999 EPERM", r, -EPERM);
}

static void test_setresuid(void) {
    puts("\n[cred/setresuid]\n");
    credtest_fork(child_setresuid);
}

/* ── setgroups/getgroups ── */

static void child_setgroups(void) {
    uint32_t g[3] = {10, 20, 30};
    long r = sc2(SYS_SETGROUPS, 3, (long)g);
    check_val("setgroups(3,...) as root", r, 0);
    long n = sc2(SYS_GETGROUPS, 0, 0);
    check_val("getgroups(0,0)=3", n, 3);
    uint32_t out[5] = {0,0,0,0,0};
    n = sc2(SYS_GETGROUPS, 5, (long)out);
    check_val("getgroups reads 3", n, 3);
    check_val("g[0]=10", out[0], 10);
    check_val("g[1]=20", out[1], 20);
    check_val("g[2]=30", out[2], 30);

    /* setgroups EPERM when unpriv */
    sc1(SYS_SETUID, NOBODY);
    uint32_t g2[1] = {99};
    r = sc2(SYS_SETGROUPS, 1, (long)g2);
    check_val("setgroups EPERM unpriv", r, -EPERM);
}

static void test_setgroups(void) {
    puts("\n[cred/setgroups]\n");
    credtest_fork(child_setgroups);
}

/* ── chmod EPERM when euid != owner ── */

static void child_chmod_eperm(void) {
    long fd = sc3(SYS_OPEN, (long)"/tmp/credtest_a", O_RDWR|O_CREAT, 0644);
    sc1(SYS_CLOSE, fd);
    /* File owned by current fsuid=0. seteuid(nobody): now non-owner */
    sc2(SYS_SETREUID, (long)-1, NOBODY);
    long r = sc2(SYS_CHMOD, (long)"/tmp/credtest_a", 0600);
    check_val("chmod EPERM when !owner", r, -EPERM);
    sc2(SYS_SETREUID, (long)-1, 0);
    sc1(SYS_UNLINK, (long)"/tmp/credtest_a");
}

static void test_chmod_eperm(void) {
    puts("\n[cred/chmod_eperm]\n");
    credtest_fork(child_chmod_eperm);
}

/* ── non-root chmod strips S_ISGID when !in_group(file->gid) ── */

static void child_chmod_sgid_strip(void) {
    long fd = sc3(SYS_OPEN, (long)"/tmp/credtest_b", O_RDWR|O_CREAT, 0644);
    sc1(SYS_CLOSE, fd);
    sc3(SYS_CHOWN, (long)"/tmp/credtest_b", NOBODY, 77); /* gid=77, not our egid */
    sc1(SYS_SETGID, NOBODY);      /* egid=nobody */
    sc2(SYS_SETREUID, (long)-1, NOBODY); /* euid=nobody (owner) */
    long r = sc2(SYS_CHMOD, (long)"/tmp/credtest_b", 02755); /* S_ISGID|0755 */
    check_val("chmod succeeds as owner", r, 0);
    struct k_stat st;
    sc4(SYS_FSTATAT, AT_FDCWD, (long)"/tmp/credtest_b", (long)&st, 0);
    /* SGID should be stripped because egid != 77 and 77 ∉ supplementary */
    check_val("S_ISGID stripped", (long)(st.st_mode & 02000), 0);
    sc2(SYS_SETREUID, (long)-1, 0);
    sc1(SYS_UNLINK, (long)"/tmp/credtest_b");
}

static void test_chmod_sgid_strip(void) {
    puts("\n[cred/chmod_sgid_strip]\n");
    credtest_fork(child_chmod_sgid_strip);
}

/* ── creat: new file gets UID=fsuid, GID=fsgid (no sgid parent) ── */

static void child_creat_owner(void) {
    sc2(SYS_MKDIR, (long)"/tmp/credtest_dir", 0777);
    sc1(SYS_SETGID, 42);
    sc2(SYS_SETREUID, (long)-1, 55);
    long fd = sc3(SYS_OPEN, (long)"/tmp/credtest_dir/f", O_RDWR|O_CREAT, 0644);
    check("creat ok", fd >= 0);
    if (fd >= 0) {
        struct k_stat st;
        sc2(SYS_FSTAT, fd, (long)&st);
        check_val("new file uid=55", (long)st.st_uid, 55);
        check_val("new file gid=42", (long)st.st_gid, 42);
        sc1(SYS_CLOSE, fd);
    }
    sc2(SYS_SETREUID, (long)-1, 0);
    sc1(SYS_UNLINK, (long)"/tmp/credtest_dir/f");
    sc1(SYS_RMDIR, (long)"/tmp/credtest_dir");
}

static void test_creat_owner(void) {
    puts("\n[cred/creat_owner]\n");
    credtest_fork(child_creat_owner);
}

/* ── creat: SGID-parent → new file inherits GID from parent ── */

static void child_creat_sgid_inherit(void) {
    sc2(SYS_MKDIR, (long)"/tmp/credtest_sgiddir", 0777);
    sc3(SYS_CHOWN, (long)"/tmp/credtest_sgiddir", 0, 88);
    sc2(SYS_CHMOD, (long)"/tmp/credtest_sgiddir", 02777); /* S_ISGID|0777 */
    sc1(SYS_SETGID, 42);  /* process egid != 88 */
    long fd = sc3(SYS_OPEN, (long)"/tmp/credtest_sgiddir/f", O_RDWR|O_CREAT, 0644);
    check("creat ok", fd >= 0);
    if (fd >= 0) {
        struct k_stat st;
        sc2(SYS_FSTAT, fd, (long)&st);
        check_val("file.gid = parent.gid", (long)st.st_gid, 88);
        sc1(SYS_CLOSE, fd);
    }
    sc1(SYS_UNLINK, (long)"/tmp/credtest_sgiddir/f");
    sc1(SYS_RMDIR, (long)"/tmp/credtest_sgiddir");
}

static void test_creat_sgid_inherit(void) {
    puts("\n[cred/creat_sgid_inherit]\n");
    credtest_fork(child_creat_sgid_inherit);
}

/* ── mkdir SGID-parent → new subdir inherits both GID and SGID bit ── */

static void child_mkdir_sgid_inherit(void) {
    sc2(SYS_MKDIR, (long)"/tmp/credtest_sgid2", 0777);
    sc3(SYS_CHOWN, (long)"/tmp/credtest_sgid2", 0, 77);
    sc2(SYS_CHMOD, (long)"/tmp/credtest_sgid2", 02777);
    sc2(SYS_MKDIR, (long)"/tmp/credtest_sgid2/sub", 0755);
    struct k_stat st;
    long r = sc2(SYS_STAT, (long)"/tmp/credtest_sgid2/sub", (long)&st);
    check_val("stat sub", r, 0);
    check_val("sub.gid = parent.gid", (long)st.st_gid, 77);
    check_val("sub has SGID", (long)(st.st_mode & 02000), 02000);
    sc1(SYS_RMDIR, (long)"/tmp/credtest_sgid2/sub");
    sc1(SYS_RMDIR, (long)"/tmp/credtest_sgid2");
}

static void test_mkdir_sgid_inherit(void) {
    puts("\n[cred/mkdir_sgid_inherit]\n");
    credtest_fork(child_mkdir_sgid_inherit);
}

/* ── path traversal requires MAY_EXEC on each dir (non-root) ── */

static void child_eacces_path(void) {
    sc2(SYS_MKDIR, (long)"/tmp/credtest_eadir", 0777);
    long fd = sc3(SYS_OPEN, (long)"/tmp/credtest_eadir/f", O_RDWR|O_CREAT, 0644);
    sc1(SYS_CLOSE, fd);
    /* Strip X-bit on directory, become nobody (non-owner): traversal EACCES */
    sc2(SYS_CHMOD, (long)"/tmp/credtest_eadir", 0644);
    sc2(SYS_SETREUID, (long)-1, NOBODY);
    struct k_stat st;
    long r = sc2(SYS_STAT, (long)"/tmp/credtest_eadir/f", (long)&st);
    check_val("stat EACCES via unreachable dir", r, -EACCES);
    r = sc2(SYS_CHMOD, (long)"/tmp/credtest_eadir/f", 0666);
    check_val("chmod EACCES via unreachable dir", r, -EACCES);
    sc2(SYS_SETREUID, (long)-1, 0);
    sc2(SYS_CHMOD, (long)"/tmp/credtest_eadir", 0777);
    sc1(SYS_UNLINK, (long)"/tmp/credtest_eadir/f");
    sc1(SYS_RMDIR, (long)"/tmp/credtest_eadir");
}

static void test_eacces_path(void) {
    puts("\n[cred/eacces_path]\n");
    credtest_fork(child_eacces_path);
}

/* ── fork inherits credentials ── */

static void child_fork_inherit(void) {
    sc1(SYS_SETUID, 123);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        long uid = sc0(SYS_GETUID);
        long euid = sc0(SYS_GETEUID);
        if (uid != 123 || euid != 123) sc1(SYS_EXIT, 1);
        sc1(SYS_EXIT, 0);
    }
    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check_val("grandchild inherits uid=123",
              (long)((status >> 8) & 0xFF), 0);
}

static void test_fork_inherit(void) {
    puts("\n[cred/fork_inherit]\n");
    credtest_fork(child_fork_inherit);
}

TEST("cred/setuid_as_root",      test_setuid_as_root);
TEST("cred/seteuid_nobody",      test_seteuid_nobody);
TEST("cred/setuid_eperm",        test_setuid_eperm);
TEST("cred/setresuid",           test_setresuid);
TEST("cred/setgroups",           test_setgroups);
TEST("cred/chmod_eperm",         test_chmod_eperm);
TEST("cred/chmod_sgid_strip",    test_chmod_sgid_strip);
TEST("cred/creat_owner",         test_creat_owner);
TEST("cred/creat_sgid_inherit",  test_creat_sgid_inherit);
TEST("cred/mkdir_sgid_inherit",  test_mkdir_sgid_inherit);
TEST("cred/eacces_path",         test_eacces_path);
TEST("cred/fork_inherit",        test_fork_inherit);
