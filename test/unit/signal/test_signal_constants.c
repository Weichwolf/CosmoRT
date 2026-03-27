#include "ktest.h"

/* ── signal constants ────────────────────────── */

static void test_signal_constants(void) {
    puts("\n[signal constants]\n");

    /* Verify a selection of signal constants are defined correctly
     * by sending signal 0 (check permission only) to self */
    long pid = sc0(SYS_GETPID);

    /* These should all succeed (sig 0 = validation only) */
    check_val("kill sig=0", sc2(SYS_KILL, pid, 0), 0);

    /* Install SIG_IGN for each signal and send it to self */
    struct { void *handler; uint64_t flags; void *restorer; uint64_t mask; } sa;
    sa.handler = (void *)1; /* SIG_IGN */
    sa.flags = 0;
    sa.restorer = 0;
    sa.mask = 0;

    /* SIGHUP=1 */
    long r = sc4(SYS_RT_SIGACTION, 1, (long)&sa, 0, 8);
    check_val("sigaction SIGHUP", r, 0);
    r = sc2(SYS_KILL, pid, 1);
    check_val("kill SIGHUP", r, 0);

    /* SIGABRT=6 */
    r = sc4(SYS_RT_SIGACTION, 6, (long)&sa, 0, 8);
    check_val("sigaction SIGABRT", r, 0);

    /* SIGBUS=7 */
    r = sc4(SYS_RT_SIGACTION, 7, (long)&sa, 0, 8);
    check_val("sigaction SIGBUS", r, 0);

    /* SIGWINCH=28 */
    r = sc4(SYS_RT_SIGACTION, 28, (long)&sa, 0, 8);
    check_val("sigaction SIGWINCH", r, 0);

    /* SIGSYS=31 */
    r = sc4(SYS_RT_SIGACTION, 31, (long)&sa, 0, 8);
    check_val("sigaction SIGSYS", r, 0);
}

TEST("signal constants", test_signal_constants);
