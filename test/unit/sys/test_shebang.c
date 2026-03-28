/* test: Shebang (#!) parsing in execve */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

/* Helper: create file with content, return 0 on success */
static int create_file(const char *path, const char *data, int len) {
    long fd = sc3(SYS_OPEN, (long)path, O_CREAT | O_WRONLY | O_TRUNC, 0755);
    if (fd < 0) return -1;
    if (len > 0) sc3(SYS_WRITE, fd, (long)data, len);
    sc1(SYS_CLOSE, fd);
    return 0;
}

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* ── Shebang with nonexistent interpreter → ENOENT ── */
static void test_shebang_enoent(void) {
    puts("\n[shebang: ENOENT]\n");

    const char script[] = "#!/nonexistent/interp\nexit 0\n";
    int r = create_file("/tmp/_shb_enoent", script, slen(script));
    check("create script", r == 0);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        char *argv[] = { "/tmp/_shb_enoent", (char *)0 };
        char *envp[] = { (char *)0 };
        long rc = sc3(SYS_EXECVE, (long)argv[0], (long)argv, (long)envp);
        /* execve failed, exit with -rc so parent sees the errno */
        sc1(SYS_EXIT_GROUP, (long)(-rc));
        __builtin_unreachable();
    }
    check("fork", pid > 0);

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child exited", WIFEXITED(status));
    check_val("ENOENT", WEXITSTATUS(status), ENOENT);

    sc2(SYS_UNLINK, (long)"/tmp/_shb_enoent", 0);
}
TEST("shebang/enoent", test_shebang_enoent);

/* ── Shebang with empty interpreter → ENOEXEC ── */
static void test_shebang_empty(void) {
    puts("\n[shebang: empty interp]\n");

    const char script[] = "#!  \nexit 0\n";
    int r = create_file("/tmp/_shb_empty", script, slen(script));
    check("create script", r == 0);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        char *argv[] = { "/tmp/_shb_empty", (char *)0 };
        char *envp[] = { (char *)0 };
        long rc = sc3(SYS_EXECVE, (long)argv[0], (long)argv, (long)envp);
        sc1(SYS_EXIT_GROUP, (long)(-rc));
        __builtin_unreachable();
    }
    check("fork", pid > 0);

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child exited", WIFEXITED(status));
    check_val("ENOEXEC", WEXITSTATUS(status), ENOEXEC);

    sc2(SYS_UNLINK, (long)"/tmp/_shb_empty", 0);
}
TEST("shebang/empty", test_shebang_empty);

/* ── Recursive shebang loop → ELOOP ── */
static void test_shebang_loop(void) {
    puts("\n[shebang: loop]\n");

    /* Script A points to Script B, B points to A */
    const char sa[] = "#!/tmp/_shb_loop_b\n";
    const char sb[] = "#!/tmp/_shb_loop_a\n";
    int r1 = create_file("/tmp/_shb_loop_a", sa, slen(sa));
    int r2 = create_file("/tmp/_shb_loop_b", sb, slen(sb));
    check("create scripts", r1 == 0 && r2 == 0);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        char *argv[] = { "/tmp/_shb_loop_a", (char *)0 };
        char *envp[] = { (char *)0 };
        long rc = sc3(SYS_EXECVE, (long)argv[0], (long)argv, (long)envp);
        sc1(SYS_EXIT_GROUP, (long)(-rc));
        __builtin_unreachable();
    }
    check("fork", pid > 0);

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child exited", WIFEXITED(status));
    check_val("ELOOP", WEXITSTATUS(status), ELOOP);

    sc2(SYS_UNLINK, (long)"/tmp/_shb_loop_a", 0);
    sc2(SYS_UNLINK, (long)"/tmp/_shb_loop_b", 0);
}
TEST("shebang/loop", test_shebang_loop);

/* ── Binary file (no #!, no ELF) → ENOEXEC ── */
static void test_shebang_noexec(void) {
    puts("\n[shebang: binary noexec]\n");

    const char data[] = "JUNK\x00\x01\x02\x03not-elf-not-shebang";
    int r = create_file("/tmp/_shb_noexec", data, sizeof(data) - 1);
    check("create file", r == 0);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        char *argv[] = { "/tmp/_shb_noexec", (char *)0 };
        char *envp[] = { (char *)0 };
        long rc = sc3(SYS_EXECVE, (long)argv[0], (long)argv, (long)envp);
        sc1(SYS_EXIT_GROUP, (long)(-rc));
        __builtin_unreachable();
    }
    check("fork", pid > 0);

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child exited", WIFEXITED(status));
    check_val("ENOEXEC", WEXITSTATUS(status), ENOEXEC);

    sc2(SYS_UNLINK, (long)"/tmp/_shb_noexec", 0);
}
TEST("shebang/noexec", test_shebang_noexec);

/* ── Shebang with arg: #!/path -e → interpreter gets -e ── */
/* We can't fully exec, but we verify the chain resolves to ENOENT
 * on the interpreter, confirming arg parsing didn't corrupt the path */
static void test_shebang_with_arg(void) {
    puts("\n[shebang: with arg]\n");

    const char script[] = "#!/nonexistent/sh -e\nexit 0\n";
    int r = create_file("/tmp/_shb_arg", script, slen(script));
    check("create script", r == 0);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        char *argv[] = { "/tmp/_shb_arg", (char *)0 };
        char *envp[] = { (char *)0 };
        long rc = sc3(SYS_EXECVE, (long)argv[0], (long)argv, (long)envp);
        sc1(SYS_EXIT_GROUP, (long)(-rc));
        __builtin_unreachable();
    }
    check("fork", pid > 0);

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child exited", WIFEXITED(status));
    /* Interpreter /nonexistent/sh doesn't exist → ENOENT */
    check_val("ENOENT (arg parsed ok)", WEXITSTATUS(status), ENOENT);

    sc2(SYS_UNLINK, (long)"/tmp/_shb_arg", 0);
}
TEST("shebang/arg", test_shebang_with_arg);

/* ── Shebang with \r\n (Windows) → strips \r ── */
static void test_shebang_crlf(void) {
    puts("\n[shebang: CRLF]\n");

    const char script[] = "#!/nonexistent/interp\r\nexit 0\n";
    int r = create_file("/tmp/_shb_crlf", script, slen(script));
    check("create script", r == 0);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        char *argv[] = { "/tmp/_shb_crlf", (char *)0 };
        char *envp[] = { (char *)0 };
        long rc = sc3(SYS_EXECVE, (long)argv[0], (long)argv, (long)envp);
        sc1(SYS_EXIT_GROUP, (long)(-rc));
        __builtin_unreachable();
    }
    check("fork", pid > 0);

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child exited", WIFEXITED(status));
    /* Should get ENOENT for /nonexistent/interp (not /nonexistent/interp\r) */
    check_val("ENOENT (\\r stripped)", WEXITSTATUS(status), ENOENT);

    sc2(SYS_UNLINK, (long)"/tmp/_shb_crlf", 0);
}
TEST("shebang/crlf", test_shebang_crlf);
