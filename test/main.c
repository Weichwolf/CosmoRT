/* CosmoRT Hardware Test — entry point
 * Unit tests run in-process.
 * Crash tests run in fork'd children so a crash kills the child, not ktest.
 * Tests self-register via .ktest linker section. */
#include "ktest.h"

int failures = 0;
int passes = 0;

extern const ktest_entry_t __start_ktest[];
extern const ktest_entry_t __stop_ktest[];

/* Run a crash test in a child process.
 * PASS = child exits 0 (all checks passed inside).
 * FAIL = child crashed or exited non-zero. */
static void run_crash_test(const char *name, void (*fn)(void)) {
    /* Save counters — child will have its own copy after fork */
    int pass_before = passes;
    int fail_before = failures;

    long pid = sc0(SYS_FORK);
    if (pid < 0) {
        fail(name, "fork failed");
        return;
    }
    if (pid == 0) {
        /* Child: run test, exit with failure count */
        failures = 0;
        passes = 0;
        fn();
        sc1(SYS_EXIT_GROUP, (long)failures);
        __builtin_unreachable();
    }

    /* Parent: wait for child */
    int status = 0;
    long w = sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
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

    for (const ktest_entry_t *t = __start_ktest; t < __stop_ktest; t++) {
        if (t->crash)
            run_crash_test(t->name, t->fn);
        else
            t->fn();
    }

    puts("\n=== ");
    put_int((long)passes); puts(" passed, ");
    put_int((long)failures); puts(" failed ===\n");
    if (failures == 0) puts("ALL PASSED\n");
    sc1(SYS_EXIT_GROUP, (long)failures);
    __builtin_unreachable();
}
