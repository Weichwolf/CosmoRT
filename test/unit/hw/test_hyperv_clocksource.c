/* Hyper-V Reference-TSC clocksource self-tests.
 *
 * Subcases 50-53 via SYS_COSMO_RT_QUERY. Auf QEMU ohne Hyper-V-Enlightenment
 * ist hyperv_detect()==0 — die meisten Tests degradieren dann zu no-op
 * success (analog HPET Boot-Probe). Subcase 53 prüft die Raw/ns-Konsistenz
 * unabhängig von der Präsenz, weil beide Werte ohne Page identisch 0 sind. */
#include "ktest.h"
#include "cosmort.h"

#define HVCS_SUB_INIT_NO_HYPERV    50
#define HVCS_SUB_REGISTER_PRESENT  51
#define HVCS_SUB_MONOTONIC         52
#define HVCS_SUB_RAW_TIMES_TICK    53

static void test_hvcs_init_no_hyperv(void) {
    puts("\n[HYPERV-CS]\n");
    check_val("hyperv_clocksource_init no-op when Hyper-V absent",
              sc2(SYS_COSMO_RT_QUERY, HVCS_SUB_INIT_NO_HYPERV, 0), 0);
}

static void test_hvcs_register_present(void) {
    check_val("clocksource hyperv_tsc registered with rating 400 under Hyper-V",
              sc2(SYS_COSMO_RT_QUERY, HVCS_SUB_REGISTER_PRESENT, 0), 0);
}

static void test_hvcs_monotonic(void) {
    check_val("read() liefert monoton steigende Werte (skip ohne Hyper-V)",
              sc2(SYS_COSMO_RT_QUERY, HVCS_SUB_MONOTONIC, 0), 0);
}

static void test_hvcs_raw_times_tick(void) {
    check_val("hyperv_tsc_time_ns == hyperv_tsc_read_raw * 100",
              sc2(SYS_COSMO_RT_QUERY, HVCS_SUB_RAW_TIMES_TICK, 0), 0);
}

TEST("hyperv_cs/init_no_hyperv",   test_hvcs_init_no_hyperv);
TEST("hyperv_cs/register_present", test_hvcs_register_present);
TEST("hyperv_cs/monotonic",        test_hvcs_monotonic);
TEST("hyperv_cs/raw_times_tick",   test_hvcs_raw_times_tick);
