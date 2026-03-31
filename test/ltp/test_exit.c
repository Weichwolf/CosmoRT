/* LTP exit01/exit02/exit_group01 — ported to ktest */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)

/* ── exit01: exit returns correct status via wait ── */

static void test_exit01(void) {
    puts("\n[ltp/exit01]\n");

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) return;

    if (pid == 0) {
        sc1(SYS_EXIT, 1);
        __builtin_unreachable();
    }

    int wstatus = 0;
    long wpid = sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check_val("wait returns correct pid", wpid, pid);
    check("child exited", WIFEXITED(wstatus));
    check_val("exit code 1", (long)WEXITSTATUS(wstatus), 1);
    check("no signal", !WIFSIGNALED(wstatus));
}

/* ── exit01: multiple exit codes ── */

static void test_exit01_codes(void) {
    puts("\n[ltp/exit01-codes]\n");

    int codes[] = { 0, 1, 42, 127, 255 };
    for (int i = 0; i < 5; i++) {
        long pid = sc0(SYS_FORK);
        if (pid < 0) { fail("fork", 0); return; }

        if (pid == 0) {
            sc1(SYS_EXIT, codes[i]);
            __builtin_unreachable();
        }

        int wstatus = 0;
        sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);

        if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != codes[i]) {
            puts("  FAIL  exit code ");
            put_int(codes[i]);
            puts(" (got=");
            put_int(WEXITSTATUS(wstatus));
            puts(")\n");
            failures++;
        } else {
            pass("exit code matches");
        }
    }
}

/* ── exit02: exit flushes file buffers ── */

static void test_exit02(void) {
    puts("\n[ltp/exit02]\n");

    /* Child creates file, writes data, exits WITHOUT close.
       Parent reads file — data should be present. */
    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) return;

    if (pid == 0) {
        long fd = sc3(SYS_OPEN, (long)"/tmp/exit02_file",
                       O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            sc3(SYS_WRITE, fd, (long)"exit02data", 10);
            /* Do NOT close — exit should flush */
        }
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));

    long fd = sc3(SYS_OPEN, (long)"/tmp/exit02_file", O_RDONLY, 0);
    check("open file", fd >= 0);
    if (fd < 0) return;

    char buf[16];
    for (int i = 0; i < 16; i++) buf[i] = 0;
    long n = sc3(SYS_READ, fd, (long)buf, 16);
    check_val("read 10 bytes", n, 10);
    check("data correct", buf[0] == 'e' && buf[9] == 'a');

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/exit02_file");
}

/* ── exit_group01: exit_group terminates process with correct code ── */

static void test_exit_group01(void) {
    puts("\n[ltp/exit_group01]\n");

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) return;

    if (pid == 0) {
        sc1(SYS_EXIT_GROUP, 4);
        __builtin_unreachable();
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));
    check_val("exit code 4", (long)WEXITSTATUS(wstatus), 4);
}

/* ── exit_group01: exit_group vs exit ── */

static void test_exit_group01_vs_exit(void) {
    puts("\n[ltp/exit_group01-vs-exit]\n");

    /* Verify exit_group works the same as exit for single-threaded */
    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) return;

    if (pid == 0) {
        /* Use SYS_EXIT (not EXIT_GROUP) */
        sc1(SYS_EXIT, 7);
        __builtin_unreachable();
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));
    check_val("exit code 7", (long)WEXITSTATUS(wstatus), 7);
}

TEST("ltp/exit01",              test_exit01);
TEST("ltp/exit01-codes",        test_exit01_codes);
TEST("ltp/exit02",              test_exit02);
TEST("ltp/exit_group01",        test_exit_group01);
TEST("ltp/exit_group01-vs-exit", test_exit_group01_vs_exit);
