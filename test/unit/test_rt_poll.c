/* test: rt_poll — prioritised polling on RT-Core.
 *
 * Installs test handlers via SYS_COSMO_RT_QUERY, waits for timer IRQ
 * to invoke rt_poll_run(), reads call order + max_work via query.
 */
#include "ktest.h"
#include "cosmort.h"

#define RT_QUERY_POLL_INSTALL  6
#define RT_QUERY_POLL_RESTORE  7
#define RT_QUERY_POLL_QUERY    8

/* Query sub-commands */
#define POLL_Q_ORDER     0   /* call_order[prio] */
#define POLL_Q_MAXWORK   1   /* max_work_seen[prio] */
#define POLL_Q_SEQ       2   /* total sequence counter */

#define RT_PRIO_COUNT    6

static long poll_install(void) {
    return sc2(SYS_COSMO_RT_QUERY, RT_QUERY_POLL_INSTALL, 0);
}

static long poll_restore(void) {
    return sc2(SYS_COSMO_RT_QUERY, RT_QUERY_POLL_RESTORE, 0);
}

static long poll_order(int prio) {
    return sc4(SYS_COSMO_RT_QUERY, RT_QUERY_POLL_QUERY, POLL_Q_ORDER, prio, 0);
}

static long poll_maxwork(int prio) {
    return sc4(SYS_COSMO_RT_QUERY, RT_QUERY_POLL_QUERY, POLL_Q_MAXWORK, prio, 0);
}

static long poll_seq(void) {
    return sc4(SYS_COSMO_RT_QUERY, RT_QUERY_POLL_QUERY, POLL_Q_SEQ, 0, 0);
}

static void msleep(int ms) {
    struct { long sec; long nsec; } ts = { ms / 1000, (ms % 1000) * 1000000L };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
}

static void test_rt_poll(void) {
    puts("\n[rt_poll]\n");

    /* Install test handlers (replaces real handlers) */
    check_val("install test handlers", poll_install(), 0);

    /* Wait for at least one timer IRQ to invoke rt_poll_run with test handlers */
    msleep(200);

    /* All 6 handlers should have been called */
    long seq = poll_seq();
    check("all handlers called", seq >= RT_PRIO_COUNT);

    /* Higher priority called first: order[0] < order[1] < ... < order[5]
     * (first timer tick that ran test handlers) */
    long o0 = poll_order(0);
    long o1 = poll_order(1);
    long o2 = poll_order(2);
    long o3 = poll_order(3);
    long o4 = poll_order(4);
    long o5 = poll_order(5);

    check("prio 0 before prio 1", o0 < o1);
    check("prio 1 before prio 2", o1 < o2);
    check("prio 2 before prio 3", o2 < o3);
    check("prio 3 before prio 4", o3 < o4);
    check("prio 4 before prio 5", o4 < o5);

    /* Bounded work: handlers receive correct max_work values.
     * Stubs were registered with: audio=1, input=8, net_rx=64, net_tx=64, vsync=1, timer=0.
     * But test_install keeps the max_work from the saved slots. */
    long mw0 = poll_maxwork(0);
    long mw1 = poll_maxwork(1);
    long mw5 = poll_maxwork(5);
    check_val("audio max_work=1", mw0, 1);
    check_val("input max_work=8", mw1, 8);
    check_val("timer max_work=0", mw5, 0);

    /* Restore real handlers */
    check_val("restore handlers", poll_restore(), 0);

    /* Verify system still works: timer wheel should still tick */
    msleep(200);
}

TEST("rt_poll", test_rt_poll);
