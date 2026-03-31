/* LTP tst_exit, tst_ncpus, tst_ncpus_conf, tst_ncpus_max — ported to ktest
 * These are LTP infrastructure utilities (ltpapicmd.c), not syscall tests.
 * tst_exit: exits with test result code — trivial
 * tst_ncpus*: read /proc/cpuinfo or sysconf(_SC_NPROCESSORS_*) — libc
 *
 * Port meaningful kernel-level equivalents:
 * - Read /proc/cpuinfo and verify it contains "processor" lines
 * - Read /proc/stat and count cpu lines
 * - Verify sched_getaffinity returns sensible mask */
#include "ktest.h"

/* ── tst_ncpus via sched_getaffinity ── */

static void test_tst_ncpus(void) {
    puts("\n[ltp/tst_ncpus]\n");

    /* sched_getaffinity(0, sizeof(mask), &mask) */
    unsigned long mask[16]; /* up to 1024 CPUs */
    for (int i = 0; i < 16; i++) mask[i] = 0;

    long r = sc3(SYS_SCHED_GETAFFINITY, 0, (long)sizeof(mask), (long)mask);
    check("sched_getaffinity succeeds", r > 0);
    if (r <= 0) return;

    /* Count set bits = online CPUs */
    int count = 0;
    for (int i = 0; i < 16; i++) {
        unsigned long m = mask[i];
        while (m) { count += (m & 1); m >>= 1; }
    }
    check("at least 1 CPU", count >= 1);
}

/* ── tst_ncpus via /proc/stat ── */

static void test_tst_ncpus_proc(void) {
    puts("\n[ltp/tst_ncpus_proc]\n");

    long fd = sc3(SYS_OPEN, (long)"/proc/stat", O_RDONLY, 0);
    check("open /proc/stat", fd >= 0);
    if (fd < 0) return;

    char buf[4096];
    long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    sc1(SYS_CLOSE, fd);
    check("read /proc/stat", n > 0);
    if (n <= 0) return;
    buf[n] = 0;

    /* Count lines starting with "cpu" followed by a digit */
    int cpus = 0;
    char *p = buf;
    while (*p) {
        if (p[0] == 'c' && p[1] == 'p' && p[2] == 'u' &&
            p[3] >= '0' && p[3] <= '9')
            cpus++;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    check("at least 1 cpu line in /proc/stat", cpus >= 1);
}

/* ── tst_exit: verify exit_group works ── */

static void test_tst_exit(void) {
    puts("\n[ltp/tst_exit]\n");

    long pid = sc5(SYS_CLONE, SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        sc1(SYS_EXIT_GROUP, 42);
    }
    check("fork", pid > 0);
    if (pid <= 0) return;

    int st;
    sc4(SYS_WAIT4, pid, (long)&st, 0, 0);
    int exited = ((st & 0x7F) == 0);
    int code = (st >> 8) & 0xFF;
    check("child exited", exited);
    check_val("exit code 42", (long)code, 42);
}

TEST("ltp/tst_ncpus",      test_tst_ncpus);
TEST("ltp/tst_ncpus_proc", test_tst_ncpus_proc);
TEST("ltp/tst_exit",       test_tst_exit);
