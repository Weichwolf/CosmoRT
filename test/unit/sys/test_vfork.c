#include "ktest.h"

/* ── vfork test ──────────────────────────────── */

static void test_vfork(void) {
    puts("\n[vfork]\n");

    /* vfork: parent blocks until child execs or exits */
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
