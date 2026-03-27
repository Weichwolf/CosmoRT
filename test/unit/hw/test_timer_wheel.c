/* test: Timer Wheel — verify RT-Core timer wheel via SYS_COSMO_RT_QUERY.
 *
 * Timer requests are asynchronous (Compute→RT via rt_channel).
 * RT-Core drains the request ring in the timer IRQ handler.
 * Tests poll tw_count() to confirm drain happened (LAPIC timer ~66ms).
 *
 * Subcommands:
 *   3 = rt_timer_request(action, ctx, timeout_ms)
 *   4 = timer_wheel_cancel(ctx)
 *   5 = timer_wheel_active_count()
 */
#include "ktest.h"
#include "cosmort.h"

#define RT_QUERY_TIMER_SET    3
#define RT_QUERY_TIMER_CANCEL 4
#define RT_QUERY_TIMER_COUNT  5

/* Timer actions (match timer_wheel.h) */
#define TW_ACTION_TCP_RETRANSMIT 1

/* Helpers */
static long tw_set(long action, long ctx, long timeout_ms) {
    return sc5(SYS_COSMO_RT_QUERY, RT_QUERY_TIMER_SET, action, ctx, timeout_ms, 0);
}

static long tw_cancel(long ctx) {
    return sc2(SYS_COSMO_RT_QUERY, RT_QUERY_TIMER_CANCEL, ctx);
}

static long tw_count(void) {
    return sc2(SYS_COSMO_RT_QUERY, RT_QUERY_TIMER_COUNT, 0);
}

static void msleep(int ms) {
    struct { long sec; long nsec; } ts = { ms / 1000, (ms % 1000) * 1000000L };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
}

/* Poll tw_count() until it exceeds threshold or timeout (ms) */
static int wait_count_above(long threshold, int timeout_ms) {
    for (int i = 0; i < timeout_ms; i += 20) {
        if (tw_count() > threshold) return 1;
        msleep(20);
    }
    return 0;
}

/* Poll tw_count() until it equals target or timeout (ms) */
static int wait_count_eq(long target, int timeout_ms) {
    for (int i = 0; i < timeout_ms; i += 20) {
        if (tw_count() == target) return 1;
        msleep(20);
    }
    return 0;
}

static void test_timer_wheel(void) {
    puts("\n[timer_wheel]\n");

    /* Let any prior timer-wheel activity settle */
    msleep(500);
    long base = tw_count();

    /* ── Timer fires after deadline ── */
    /* Magic pointer as context — retransmit handler ignores invalid conns */
    long ctx1 = 0xDEAD0001;
    long r = tw_set(TW_ACTION_TCP_RETRANSMIT, ctx1, 2000);
    check_val("timer_set returns 0", r, 0);

    /* Wait for RT-Core to drain request ring into wheel (poll) */
    check("timer active after set", wait_count_above(base, 1000));

    /* Wait for timer to fire (2000ms timeout + margin) */
    check("timer freed after fire", wait_count_eq(base, 4000));

    /* ── Timer cancel before deadline ── */
    long ctx2 = 0xDEAD0002;
    r = tw_set(TW_ACTION_TCP_RETRANSMIT, ctx2, 30000);
    check_val("timer_set long returns 0", r, 0);

    check("timer active before cancel", wait_count_above(base, 1000));

    /* Cancel — direct call, entry is already in wheel after drain */
    long cancelled = tw_cancel(ctx2);
    check("cancel returns >0", cancelled > 0);

    long cnt = tw_count();
    check_val("timer gone after cancel", cnt, base);

    /* ── Multiple timers ── */
    long ctx3 = 0xDEAD0003;
    long ctx4 = 0xDEAD0004;
    tw_set(TW_ACTION_TCP_RETRANSMIT, ctx3, 30000);
    tw_set(TW_ACTION_TCP_RETRANSMIT, ctx4, 30000);

    /* Wait for both to be drained */
    for (int i = 0; i < 2000; i += 20) {
        if (tw_count() >= base + 2) break;
        msleep(20);
    }
    cnt = tw_count();
    check("two timers active", cnt >= base + 2);

    tw_cancel(ctx3);
    tw_cancel(ctx4);
    cnt = tw_count();
    check_val("all cancelled", cnt, base);

    /* ── Cancel non-existent context ── */
    cancelled = tw_cancel(0xBAADF00D);
    check_val("cancel nonexistent returns 0", cancelled, 0);
}

static void test_timer_wheel_pool(void) {
    puts("\n[timer_wheel_pool]\n");

    /* Let any prior activity settle */
    msleep(500);
    long base = tw_count();

    /* ── 128+ timers simultaneously active ── */
    #define POOL_N 128
    for (int i = 0; i < POOL_N; i++) {
        long ctx = 0xF000000 + i;
        long r = tw_set(TW_ACTION_TCP_RETRANSMIT, ctx, 30000);
        check_val("pool set ok", r, 0);
    }

    /* Wait for all to be drained into wheel */
    for (int i = 0; i < 3000; i += 20) {
        if (tw_count() >= base + POOL_N) break;
        msleep(20);
    }
    long cnt = tw_count();
    check("128 timers active", cnt >= base + POOL_N);

    /* ── Alloc/free/re-alloc roundtrip ── */
    /* Cancel half, then re-add — exercises free-stack recycling */
    for (int i = 0; i < POOL_N / 2; i++) {
        long ctx = 0xF000000 + i;
        tw_cancel(ctx);
    }
    cnt = tw_count();
    check("64 remain after half cancel", cnt >= base + POOL_N / 2);

    /* Re-add the cancelled ones */
    for (int i = 0; i < POOL_N / 2; i++) {
        long ctx = 0xF100000 + i;
        long r = tw_set(TW_ACTION_TCP_RETRANSMIT, ctx, 30000);
        check_val("re-alloc ok", r, 0);
    }

    for (int i = 0; i < 3000; i += 20) {
        if (tw_count() >= base + POOL_N) break;
        msleep(20);
    }
    cnt = tw_count();
    check("128 timers active after re-alloc", cnt >= base + POOL_N);

    /* Cleanup: cancel all */
    for (int i = POOL_N / 2; i < POOL_N; i++) {
        long ctx = 0xF000000 + i;
        tw_cancel(ctx);
    }
    for (int i = 0; i < POOL_N / 2; i++) {
        long ctx = 0xF100000 + i;
        tw_cancel(ctx);
    }
    cnt = tw_count();
    check_val("all pool timers cleaned", cnt, base);
}

TEST("timer_wheel", test_timer_wheel);
TEST("timer_wheel_pool", test_timer_wheel_pool);
