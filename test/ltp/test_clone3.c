/* LTP clone301/clone302 — ported to ktest */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

#define CSIGNAL         0x000000FF

/* clone3 args — must match Linux struct clone_args (64 bytes min) */
struct clone_args {
    uint64_t flags;
    uint64_t pidfd;
    uint64_t child_tid;
    uint64_t parent_tid;
    uint64_t exit_signal;
    uint64_t stack;
    uint64_t stack_size;
    uint64_t tls;
    uint64_t set_tid;
    uint64_t set_tid_size;
    uint64_t cgroup;
};

#define CLONE_ARGS_SIZE_MIN 64

/* ── clone301: basic clone3 — child exits, parent waits ── */

static void test_clone301(void) {
    puts("\n[ltp/clone301]\n");

    struct clone_args args;
    for (int i = 0; i < (int)sizeof(args); i++)
        ((char *)&args)[i] = 0;

    args.exit_signal = SIGCHLD;

    long pid = sc2(SYS_CLONE3, (long)&args, (long)sizeof(args));
    check("clone3", pid >= 0);
    if (pid < 0) return;

    if (pid == 0) {
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    int wstatus = 0;
    long wpid = sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check_val("wait returns child pid", wpid, pid);
    check("child exited cleanly", WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);
}

/* ── clone301: clone3 with CLONE_FS ── */

static void test_clone301_fs(void) {
    puts("\n[ltp/clone301-fs]\n");

    struct clone_args args;
    for (int i = 0; i < (int)sizeof(args); i++)
        ((char *)&args)[i] = 0;

    args.flags = CLONE_FS;
    args.exit_signal = SIGCHLD;

    long pid = sc2(SYS_CLONE3, (long)&args, (long)sizeof(args));
    check("clone3 CLONE_FS", pid >= 0);
    if (pid < 0) return;

    if (pid == 0) {
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);
}

/* ── clone301: clone3 with custom exit_signal ── */

static void test_clone301_sigusr2(void) {
    puts("\n[ltp/clone301-sigusr2]\n");

    struct clone_args args;
    for (int i = 0; i < (int)sizeof(args); i++)
        ((char *)&args)[i] = 0;

    args.exit_signal = SIGUSR2;

    long pid = sc2(SYS_CLONE3, (long)&args, (long)sizeof(args));
    check("clone3 SIGUSR2", pid >= 0);
    if (pid < 0) return;

    if (pid == 0) {
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child reaped", WIFEXITED(wstatus));
}

/* ── clone302: error cases ── */

static void test_clone302_zero_size(void) {
    puts("\n[ltp/clone302-zero-size]\n");

    struct clone_args args;
    for (int i = 0; i < (int)sizeof(args); i++)
        ((char *)&args)[i] = 0;
    args.exit_signal = SIGCHLD;

    long r = sc2(SYS_CLONE3, (long)&args, 0);
    check_val("size=0 EINVAL", r, -EINVAL);
}

static void test_clone302_short_size(void) {
    puts("\n[ltp/clone302-short-size]\n");

    struct clone_args args;
    for (int i = 0; i < (int)sizeof(args); i++)
        ((char *)&args)[i] = 0;
    args.exit_signal = SIGCHLD;

    long r = sc2(SYS_CLONE3, (long)&args, CLONE_ARGS_SIZE_MIN - 1);
    check_val("short size EINVAL", r, -EINVAL);
}

static void test_clone302_sighand_no_vm(void) {
    puts("\n[ltp/clone302-sighand-no-vm]\n");

    struct clone_args args;
    for (int i = 0; i < (int)sizeof(args); i++)
        ((char *)&args)[i] = 0;

    args.flags = CLONE_SIGHAND;
    args.exit_signal = SIGCHLD;

    long r = sc2(SYS_CLONE3, (long)&args, (long)sizeof(args));
    check_val("CLONE_SIGHAND without CLONE_VM EINVAL", r, -EINVAL);
}

static void test_clone302_thread_no_sighand(void) {
    puts("\n[ltp/clone302-thread-no-sighand]\n");

    struct clone_args args;
    for (int i = 0; i < (int)sizeof(args); i++)
        ((char *)&args)[i] = 0;

    args.flags = CLONE_THREAD;
    args.exit_signal = SIGCHLD;

    long r = sc2(SYS_CLONE3, (long)&args, (long)sizeof(args));
    check_val("CLONE_THREAD without CLONE_SIGHAND EINVAL", r, -EINVAL);
}

static void test_clone302_fs_newns(void) {
    puts("\n[ltp/clone302-fs-newns]\n");

    struct clone_args args;
    for (int i = 0; i < (int)sizeof(args); i++)
        ((char *)&args)[i] = 0;

    args.flags = CLONE_FS | CLONE_NEWNS;
    args.exit_signal = SIGCHLD;

    long r = sc2(SYS_CLONE3, (long)&args, (long)sizeof(args));
    check_val("CLONE_FS|CLONE_NEWNS EINVAL", r, -EINVAL);
}

static void test_clone302_invalid_signal(void) {
    puts("\n[ltp/clone302-invalid-signal]\n");

    struct clone_args args;
    for (int i = 0; i < (int)sizeof(args); i++)
        ((char *)&args)[i] = 0;

    args.exit_signal = CSIGNAL + 1;

    long r = sc2(SYS_CLONE3, (long)&args, (long)sizeof(args));
    check_val("invalid signal EINVAL", r, -EINVAL);
}

static void test_clone302_zero_stack_size(void) {
    puts("\n[ltp/clone302-zero-stack-size]\n");

    long stack_var;

    struct clone_args args;
    for (int i = 0; i < (int)sizeof(args); i++)
        ((char *)&args)[i] = 0;

    args.exit_signal = SIGCHLD;
    args.stack = (uint64_t)&stack_var;
    args.stack_size = 0;

    long r = sc2(SYS_CLONE3, (long)&args, (long)sizeof(args));
    check_val("stack set, stack_size=0 EINVAL", r, -EINVAL);
}

static void test_clone302_invalid_stack(void) {
    puts("\n[ltp/clone302-invalid-stack]\n");

    struct clone_args args;
    for (int i = 0; i < (int)sizeof(args); i++)
        ((char *)&args)[i] = 0;

    args.exit_signal = SIGCHLD;
    args.stack = 0;
    args.stack_size = 4;

    long r = sc2(SYS_CLONE3, (long)&args, (long)sizeof(args));
    check_val("no stack but stack_size EINVAL", r, -EINVAL);
}

TEST("ltp/clone301",                    test_clone301);
TEST("ltp/clone301-fs",                 test_clone301_fs);
TEST("ltp/clone301-sigusr2",            test_clone301_sigusr2);
TEST("ltp/clone302-zero-size",          test_clone302_zero_size);
TEST("ltp/clone302-short-size",         test_clone302_short_size);
TEST("ltp/clone302-sighand-no-vm",      test_clone302_sighand_no_vm);
TEST("ltp/clone302-thread-no-sighand",  test_clone302_thread_no_sighand);
TEST("ltp/clone302-fs-newns",           test_clone302_fs_newns);
TEST("ltp/clone302-invalid-signal",     test_clone302_invalid_signal);
TEST("ltp/clone302-zero-stack-size",    test_clone302_zero_stack_size);
TEST("ltp/clone302-invalid-stack",      test_clone302_invalid_stack);
