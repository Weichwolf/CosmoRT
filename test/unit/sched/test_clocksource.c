/* CosmoRT clocksource / clock_event core tests.
 *
 * Kernel-internal API is not syscall-reachable directly. Each TEST invokes
 * a SYS_COSMO_RT_QUERY subcase (10-19) whose kernel-side helper registers
 * fake clocksources, asserts invariants, and unregisters before returning.
 * Return value: 0 on success, negative on failure. */
#include "ktest.h"
#include "cosmort.h"

#define CS_SUB_RATING_SORT        10
#define CS_SUB_REGISTER_HIGHER    11
#define CS_SUB_REGISTER_LOWER     12
#define CS_SUB_UNREGISTER_CURRENT 13
#define CS_SUB_UNREGISTER_NONCUR  14
#define CS_SUB_MULT_SHIFT_PM      15
#define CS_SUB_MULT_SHIFT_TSC     16
#define CS_SUB_MASK_WRAP          17
#define CS_SUB_MONOTONIC          18
#define CS_SUB_CLOCK_EVENT        19

static void test_cs_rating_sort(void) {
    puts("\n[Clocksource]\n");
    long r = sc2(SYS_COSMO_RT_QUERY, CS_SUB_RATING_SORT, 0);
    check_val("rating-descending insertion order", r, 0);
}

static void test_cs_register_higher(void) {
    long r = sc2(SYS_COSMO_RT_QUERY, CS_SUB_REGISTER_HIGHER, 0);
    check_val("higher rating switches current", r, 0);
}

static void test_cs_register_lower(void) {
    long r = sc2(SYS_COSMO_RT_QUERY, CS_SUB_REGISTER_LOWER, 0);
    check_val("lower rating keeps current", r, 0);
}

static void test_cs_unregister_current(void) {
    long r = sc2(SYS_COSMO_RT_QUERY, CS_SUB_UNREGISTER_CURRENT, 0);
    check_val("unregister current falls back", r, 0);
}

static void test_cs_unregister_noncurrent(void) {
    long r = sc2(SYS_COSMO_RT_QUERY, CS_SUB_UNREGISTER_NONCUR, 0);
    check_val("unregister non-current preserves", r, 0);
}

static void test_cs_mult_shift_pm(void) {
    long r = sc2(SYS_COSMO_RT_QUERY, CS_SUB_MULT_SHIFT_PM, 0);
    check_val("mult/shift for ACPI-PM (3.579545 MHz)", r, 0);
}

static void test_cs_mult_shift_tsc(void) {
    long r = sc2(SYS_COSMO_RT_QUERY, CS_SUB_MULT_SHIFT_TSC, 0);
    check_val("mult/shift for TSC (2.4 GHz)", r, 0);
}

static void test_cs_mask_wrap(void) {
    long r = sc2(SYS_COSMO_RT_QUERY, CS_SUB_MASK_WRAP, 0);
    check_val("32-bit mask wrap delta correct", r, 0);
}

static void test_cs_monotonic(void) {
    long r = sc2(SYS_COSMO_RT_QUERY, CS_SUB_MONOTONIC, 0);
    check_val("consecutive reads non-decreasing", r, 0);
}

static void test_cs_clock_event(void) {
    long r = sc2(SYS_COSMO_RT_QUERY, CS_SUB_CLOCK_EVENT, 0);
    check_val("clock_event register+set_oneshot forwards delta", r, 0);
}

TEST("clocksource/rating_sort",       test_cs_rating_sort);
TEST("clocksource/register_higher",   test_cs_register_higher);
TEST("clocksource/register_lower",    test_cs_register_lower);
TEST("clocksource/unregister_current",test_cs_unregister_current);
TEST("clocksource/unregister_noncur", test_cs_unregister_noncurrent);
TEST("clocksource/mult_shift_pm",     test_cs_mult_shift_pm);
TEST("clocksource/mult_shift_tsc",    test_cs_mult_shift_tsc);
TEST("clocksource/mask_wrap",         test_cs_mask_wrap);
TEST("clocksource/monotonic",         test_cs_monotonic);
TEST("clocksource/clock_event",       test_cs_clock_event);
