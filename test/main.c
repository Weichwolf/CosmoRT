/* CosmoRT Hardware Test — entry point
 * Unit tests run in-process.
 * Crash tests run in fork'd children so a crash kills the child, not ktest. */
#include "ktest.h"

int failures = 0;
int passes = 0;

/* ── Unit tests ── */
extern void test_identity(void);
extern void test_memory(void);
extern void test_tls(void);
extern void test_time(void);
extern void test_random(void);
extern void test_vfs(void);
extern void test_procfs(void);
extern void test_threads(void);
extern void test_pci(void);
extern void test_security(void);
extern void test_yield(void);
extern void test_signals(void);
extern void test_vm_patterns(void);

/* ── Crash tests ── */
extern void test_v8_cage(void);
extern void test_oom(void);
extern void test_fork_bomb(void);
extern void test_stack(void);
extern void test_badptr(void);

/* Run a crash test in a child process.
 * PASS = child exits 0 (all checks passed inside).
 * FAIL = child crashed or exited non-zero. */
static void run_crash_test(const char *name, void (*fn)(void)) {
    /* Save counters — child will have its own copy after fork */
    int pass_before = passes;
    int fail_before = failures;

    long pid = sc0(SYS_fork);
    if (pid < 0) {
        fail(name, "fork failed");
        return;
    }
    if (pid == 0) {
        /* Child: run test, exit with failure count */
        failures = 0;
        passes = 0;
        fn();
        sc1(SYS_exit_group, (long)failures);
        __builtin_unreachable();
    }

    /* Parent: wait for child */
    int status = 0;
    long w = sc4(SYS_wait4, pid, (long)&status, 0, 0);
    if (w < 0) {
        fail(name, "wait4 failed");
        return;
    }

    /* Decode status: exit code is in bits 15:8 on Linux,
     * signal in bits 6:0. Our kernel returns raw exit code from do_wait4. */
    int exit_code = status;

    /* The child printed its own PASS/FAIL lines.
     * We just track whether the whole suite survived. */
    if (exit_code == 0) {
        /* Child's internal checks all passed */
        puts("  --- "); puts(name); puts(": child OK ---\n");
    } else if (exit_code > 128) {
        /* Killed by signal (128+sig convention) or raw signal number */
        puts("  --- "); puts(name); puts(": child CRASHED (status=");
        put_int(exit_code); puts(") ---\n");
        fail(name, "child crashed");
        return;
    } else if (exit_code > 0) {
        puts("  --- "); puts(name); puts(": child had ");
        put_int(exit_code); puts(" failures ---\n");
    }

    /* Since child counters are in child address space, we need to
     * account for them in the parent. We trust the child's exit code
     * as the failure count. The child printed its own PASS lines,
     * but the parent didn't see the counter increments.
     * Approximate: add reasonable counts based on exit code. */

    /* Restore counters — fork'd child's increments are lost.
     * We count one PASS per crash-suite if exit_code == 0,
     * or one FAIL if the child died or had failures. */
    passes = pass_before;
    failures = fail_before;

    if (exit_code == 0) {
        pass(name);
    } else {
        fail(name, "child failures or crash");
    }
}

void _start(void) {
    puts("\n=== CosmoRT Hardware Test ===\n");

    /* ── Unit Suite ── */
    puts("\n--- Unit Tests ---\n");
    test_identity();
    test_memory();
    test_tls();
    test_time();
    test_random();
    test_vfs();
    test_procfs();
    test_threads();
    test_pci();
    test_security();
    test_yield();
    test_signals();
    test_vm_patterns();

    /* ── Crash Suite ── */
    puts("\n--- Crash Tests ---\n");
    run_crash_test("crash/v8_cage",   test_v8_cage);
    run_crash_test("crash/oom",       test_oom);
    run_crash_test("crash/fork_bomb", test_fork_bomb);
    run_crash_test("crash/stack",     test_stack);
    run_crash_test("crash/badptr",    test_badptr);

    puts("\n=== ");
    put_int((long)passes); puts(" passed, ");
    put_int((long)failures); puts(" failed ===\n");

    if (failures == 0)
        puts("ALL PASSED\n");

    sc1(SYS_exit_group, (long)failures);
    __builtin_unreachable();
}
