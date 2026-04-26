/* Phase 10.2a — try_to_wake_up(mask), wait_func_t, signal_wake_up,
 * autoremove_wake_function. Kernel-internal selftests via SYS_COSMO_RT_QUERY
 * subcases 200-203. The kernel side allocates a slab thread, manipulates
 * t->state directly, exercises the public wake APIs, and returns 0 on
 * success or a negative error code identifying the failing assertion. */
#include "ktest.h"
#include "cosmort.h"

#define WQ_SUB_MASK_FILTER       200
#define WQ_SUB_FUNC_CALLED       201
#define WQ_SUB_SIGNAL_WAKE_UP    202
#define WQ_SUB_AUTOREMOVE        203

static void test_wq_mask_filter(void) {
    puts("\n[Wake/Mask]\n");
    check_val("try_to_wake_up_mask_filter",
              sc2(SYS_COSMO_RT_QUERY, WQ_SUB_MASK_FILTER, 0), 0);
}

static void test_wq_func_called(void) {
    check_val("wait_queue_entry_func_called",
              sc2(SYS_COSMO_RT_QUERY, WQ_SUB_FUNC_CALLED, 0), 0);
}

static void test_wq_signal_wake_up_basic(void) {
    check_val("signal_wake_up_basic",
              sc2(SYS_COSMO_RT_QUERY, WQ_SUB_SIGNAL_WAKE_UP, 0), 0);
}

static void test_wq_autoremove_returns_one(void) {
    check_val("autoremove_wake_function_returns_one",
              sc2(SYS_COSMO_RT_QUERY, WQ_SUB_AUTOREMOVE, 0), 0);
}

TEST("wake/mask_filter",        test_wq_mask_filter);
TEST("wake/func_called",        test_wq_func_called);
TEST("wake/signal_wake_up",     test_wq_signal_wake_up_basic);
TEST("wake/autoremove_one",     test_wq_autoremove_returns_one);
