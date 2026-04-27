/* Class B: RLIMIT-Enforcement.
 *
 * Diese Session hat RLIMIT_NPROC, RLIMIT_FSIZE, RLIMIT_CPU, RLIMIT_NOFILE
 * verdrahtet. Tests muessen Enforcement + SIGXFSZ/SIGXCPU-Delivery
 * provozieren. */
#include "ktest.h"

#define T_RLIMIT_CPU     0
#define T_RLIMIT_FSIZE   1
#define T_RLIMIT_NPROC   6
#define T_RLIMIT_NOFILE  7

#define RLIM_INF         (~0UL)

struct t_rlim { unsigned long cur, max; };

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)

static long set_rlim(int res, unsigned long cur, unsigned long max) {
    struct t_rlim r = { cur, max };
    return sc4(SYS_PRLIMIT64, 0, res, (long)&r, 0);
}

static long get_rlim(int res, struct t_rlim *out) {
    return sc4(SYS_PRLIMIT64, 0, res, 0, (long)out);
}

static void test_rlimit_nproc_enforced(void) {
    puts("\n[RLIMIT_NPROC_ENFORCED]\n");

    long pid = sc0(SYS_FORK);
    check("fork probe", pid >= 0);
    if (pid < 0) return;
    if (pid == 0) {
        long r = set_rlim(T_RLIMIT_NPROC, 10, RLIM_INF);
        if (r != 0) sc1(SYS_EXIT_GROUP, 10);

        int forked = 0, got_eagain = 0;
        long kids[64];
        for (int i = 0; i < 40; i++) {
            long c = sc0(SYS_FORK);
            if (c == 0) {
                for (volatile int j = 0; j < 10000000; j++)
                    __asm__ volatile("pause");
                sc1(SYS_EXIT_GROUP, 0);
                __builtin_unreachable();
            }
            if (c < 0) { if (c == -EAGAIN) got_eagain = 1; break; }
            kids[forked++] = c;
        }

        int exit_code = 0;
        if (!got_eagain) exit_code = 20;
        else if (forked > 20) exit_code = 21;

        for (int i = 0; i < forked; i++) {
            sc2(SYS_KILL, kids[i], 9);
            sc4(SYS_WAIT4, kids[i], 0, 0, 0);
        }

        sc1(SYS_EXIT_GROUP, exit_code);
        __builtin_unreachable();
    }

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child exited", WIFEXITED(status));
    check_val("NPROC enforcement child exit=0", (long)WEXITSTATUS(status), 0);
}

static void test_rlimit_fsize_efbig(void) {
    puts("\n[RLIMIT_FSIZE_EFBIG]\n");

    long pid = sc0(SYS_FORK);
    check("fork probe", pid >= 0);
    if (pid < 0) return;
    if (pid == 0) {
        struct k_sigaction ign = { (void *)1, 0, 0, 0 };
        sc4(SYS_RT_SIGACTION, SIGXFSZ, (long)&ign, 0, 8);

        if (set_rlim(T_RLIMIT_FSIZE, 1024, RLIM_INF) != 0)
            sc1(SYS_EXIT_GROUP, 10);

        long fd = sc3(SYS_OPEN, (long)"/tmp/rlim_fsize",
                      O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) sc1(SYS_EXIT_GROUP, 11);

        char buf[2000];
        for (int i = 0; i < 2000; i++) buf[i] = 'A';
        long w1 = sc3(SYS_WRITE, fd, (long)buf, 2000);
        long w2 = sc3(SYS_WRITE, fd, (long)buf, 2000);

        sc1(SYS_CLOSE, fd);
        sc1(SYS_UNLINK, (long)"/tmp/rlim_fsize");

        int code = 0;
        if (w1 != 1024 && w1 != 2000) code = 20;
        if (w1 == 1024) {
            if (w2 != -EFBIG) code = 21;
        }
        sc1(SYS_EXIT_GROUP, code);
        __builtin_unreachable();
    }

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child exited", WIFEXITED(status));
    check_val("RLIMIT_FSIZE enforced", (long)WEXITSTATUS(status), 0);
}

static void test_rlimit_fsize_sigxfsz(void) {
    puts("\n[RLIMIT_FSIZE_SIGXFSZ]\n");

    long pid = sc0(SYS_FORK);
    check("fork probe", pid >= 0);
    if (pid < 0) return;
    if (pid == 0) {
        struct k_sigaction dfl = { 0, 0, 0, 0 };
        sc4(SYS_RT_SIGACTION, SIGXFSZ, (long)&dfl, 0, 8);

        if (set_rlim(T_RLIMIT_FSIZE, 100, RLIM_INF) != 0)
            sc1(SYS_EXIT_GROUP, 10);

        long fd = sc3(SYS_OPEN, (long)"/tmp/rlim_fsize_sig",
                      O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) sc1(SYS_EXIT_GROUP, 11);

        char buf[500];
        for (int i = 0; i < 500; i++) buf[i] = 'B';
        sc3(SYS_WRITE, fd, (long)buf, 500);
        sc3(SYS_WRITE, fd, (long)buf, 500);

        sc1(SYS_CLOSE, fd);
        sc1(SYS_UNLINK, (long)"/tmp/rlim_fsize_sig");
        sc1(SYS_EXIT_GROUP, 99);
        __builtin_unreachable();
    }

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child killed by signal", WIFSIGNALED(status));
    check_val("SIGXFSZ delivered", (long)WTERMSIG(status), (long)SIGXFSZ);
}

static void test_rlimit_nofile_fork_inherit(void) {
    puts("\n[RLIMIT_NOFILE_FORK_INHERIT]\n");

    struct t_rlim saved;
    get_rlim(T_RLIMIT_NOFILE, &saved);

    set_rlim(T_RLIMIT_NOFILE, 64, 65536);

    long pid = sc0(SYS_FORK);
    check("fork probe", pid >= 0);
    if (pid < 0) { set_rlim(T_RLIMIT_NOFILE, saved.cur, saved.max); return; }

    if (pid == 0) {
        struct t_rlim child_lim;
        get_rlim(T_RLIMIT_NOFILE, &child_lim);
        sc1(SYS_EXIT_GROUP, (int)(child_lim.cur == 64 ? 0 : 42));
        __builtin_unreachable();
    }

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    set_rlim(T_RLIMIT_NOFILE, saved.cur, saved.max);
    check_val("child inherits RLIMIT_NOFILE", (long)WEXITSTATUS(status), 0);
}

static void test_rlimit_cpu_soft_xcpu(void) {
    puts("\n[RLIMIT_CPU_SOFT_XCPU]\n");

    long pid = sc0(SYS_FORK);
    check("fork probe", pid >= 0);
    if (pid < 0) return;
    if (pid == 0) {
        volatile int *shared_ok = (volatile int *)sc6(SYS_MMAP, 0, 4096,
            PROT_RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if ((long)shared_ok <= 0) sc1(SYS_EXIT_GROUP, 10);
        *shared_ok = 0;

        struct k_sigaction dfl = { 0, 0, 0, 0 };
        sc4(SYS_RT_SIGACTION, SIGXCPU, (long)&dfl, 0, 8);

        if (set_rlim(T_RLIMIT_CPU, 1, 3) != 0)
            sc1(SYS_EXIT_GROUP, 11);

        for (volatile uint64_t i = 0; i < (uint64_t)1e10; i++) {
            if (i % 100000 == 0) __asm__ volatile("");
        }
        sc1(SYS_EXIT_GROUP, 99);
        __builtin_unreachable();
    }

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("cpu-limit child exited", WIFEXITED(status) || WIFSIGNALED(status));
    if (WIFSIGNALED(status)) {
        check("killed by XCPU or KILL",
              WTERMSIG(status) == SIGXCPU || WTERMSIG(status) == SIGKILL);
    }
}

/* RLIMIT_NPROC=0 must forbid fork (Linux ABI). Earlier the kernel mapped
 * literal 0 to "unlimited" so fork still succeeded. Regression: fork must
 * return -EAGAIN and prlimit64 readback must mirror the literal 0. */
static void test_rlimit_nproc_zero(void) {
    puts("\n[RLIMIT_NPROC_ZERO_FORBIDS_FORK]\n");

    long pid = sc0(SYS_FORK);
    check("fork probe", pid >= 0);
    if (pid < 0) return;
    if (pid == 0) {
        long r = set_rlim(T_RLIMIT_NPROC, 0, RLIM_INF);
        if (r != 0) sc1(SYS_EXIT_GROUP, 10);

        struct t_rlim got = { 1, 1 };
        get_rlim(T_RLIMIT_NPROC, &got);
        if (got.cur != 0) sc1(SYS_EXIT_GROUP, 11);

        long c = sc0(SYS_FORK);
        if (c >= 0) {
            if (c == 0) sc1(SYS_EXIT_GROUP, 0);
            sc4(SYS_WAIT4, c, 0, 0, 0);
            sc1(SYS_EXIT_GROUP, 12);
        }
        if (c != -EAGAIN) sc1(SYS_EXIT_GROUP, 13);
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child exited", WIFEXITED(status));
    check_val("NPROC=0 forbids fork", (long)WEXITSTATUS(status), 0);
}

TEST("rlimit/nproc",            test_rlimit_nproc_enforced);
TEST("rlimit/nproc_zero",       test_rlimit_nproc_zero);
TEST("rlimit/fsize_efbig",      test_rlimit_fsize_efbig);
TEST("rlimit/fsize_sigxfsz",    test_rlimit_fsize_sigxfsz);
TEST("rlimit/nofile_inherit",   test_rlimit_nofile_fork_inherit);
TEST("rlimit/cpu_soft",         test_rlimit_cpu_soft_xcpu);
