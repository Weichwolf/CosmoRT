/* LTP abort01 — ported to ktest */
#include "ktest.h"

#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)

/* abort01: child sends SIGABRT to itself, parent verifies signal death */

static void test_abort01(void) {
    puts("\n[ltp/abort01]\n");

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) return;

    if (pid == 0) {
        /* child: send SIGABRT to self (equivalent to abort()) */
        long self = sc0(SYS_GETPID);
        sc2(SYS_KILL, self, SIGABRT);
        /* should not reach here */
        sc1(SYS_EXIT, 99);
    }

    /* parent: wait for child */
    int status = 0;
    long w = sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("wait4", w == pid);
    check("child signaled", WIFSIGNALED(status));
    if (WIFSIGNALED(status))
        check_val("SIGABRT (6)", (long)WTERMSIG(status), SIGABRT);
}

TEST("ltp/abort01", test_abort01);
