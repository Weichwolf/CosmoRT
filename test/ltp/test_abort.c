/* LTP abort01 — ported to ktest */
#include "ktest.h"

#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)
#define WCOREDUMP(s)    ((s) & 0x80)

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
    check("WCOREDUMP fuer SIGABRT", WCOREDUMP(status) != 0);
}

/* abort01-coredump-signals: WCOREDUMP fuer alle core-erzeugenden Signale */

static void test_abort01_coredump_signals(void) {
    puts("\n[ltp/abort01-coredump-signals]\n");

    int core_sigs[] = { SIGABRT, SIGQUIT, SIGSEGV, SIGBUS,
                        SIGFPE, SIGILL, SIGTRAP, SIGXFSZ,
                        SIGXCPU, SIGSYS };
    int no_core[] = { SIGHUP, SIGINT, SIGTERM, SIGUSR1, SIGUSR2 };

    for (unsigned i = 0; i < sizeof(core_sigs)/sizeof(core_sigs[0]); i++) {
        long pid = sc0(SYS_FORK);
        if (pid == 0) {
            long self = sc0(SYS_GETPID);
            sc2(SYS_KILL, self, core_sigs[i]);
            sc1(SYS_EXIT, 99);
        }
        int status = 0;
        sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
        check("WCOREDUMP gesetzt (core-sig)", WCOREDUMP(status) != 0);
    }

    for (unsigned i = 0; i < sizeof(no_core)/sizeof(no_core[0]); i++) {
        long pid = sc0(SYS_FORK);
        if (pid == 0) {
            long self = sc0(SYS_GETPID);
            sc2(SYS_KILL, self, no_core[i]);
            sc1(SYS_EXIT, 99);
        }
        int status = 0;
        sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
        check("WCOREDUMP nicht gesetzt (kein core-sig)", WCOREDUMP(status) == 0);
    }
}

TEST("ltp/abort01",                  test_abort01);
TEST("ltp/abort01-coredump-signals", test_abort01_coredump_signals);
