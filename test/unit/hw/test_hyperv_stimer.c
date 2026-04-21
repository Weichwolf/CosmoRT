/* Hyper-V STimer (STIMER0) clock_event self-tests.
 *
 * Subcases 70-74 via SYS_COSMO_RT_QUERY.
 *   70: init-no-hyperv — kein clock_event registriert wenn Hyper-V fehlt
 *   71: CONFIG-Builder — Direct-Mode- und SynIC-Payload pur in Software
 *   72: Deadline-Konversion ns→ticks + Min-Delta-Clamp
 *   73: clock_event "hv-stimer" rating 400 registriert (skip ohne Hyper-V)
 *   74: shutdown() idempotent (skip ohne Hyper-V)
 *
 * Auf QEMU-TCG ohne Hyper-V-Enlightenment degradieren 73/74 zu no-op success
 * — selbe Semantik wie die TSC-Page- und KVM-clock-Tests. 70-72 laufen
 * unabhängig von der Host-Präsenz. */
#include "ktest.h"
#include "cosmort.h"

#define HVST_SUB_INIT_NO_HYPERV    70
#define HVST_SUB_CONFIG_BUILD      71
#define HVST_SUB_DEADLINE_CONVERT  72
#define HVST_SUB_REGISTER_PRESENT  73
#define HVST_SUB_SHUTDOWN          74

static void test_hvst_init_no_hyperv(void) {
    puts("\n[HYPERV-STIMER]\n");
    check_val("hyperv_stimer_init no-op when Hyper-V absent",
              sc2(SYS_COSMO_RT_QUERY, HVST_SUB_INIT_NO_HYPERV, 0), 0);
}

static void test_hvst_config_build(void) {
    check_val("CONFIG-MSR encodes ENABLE|DIRECT|VECTOR resp. SINTX",
              sc2(SYS_COSMO_RT_QUERY, HVST_SUB_CONFIG_BUILD, 0), 0);
}

static void test_hvst_deadline_convert(void) {
    check_val("deadline = now + delta_ns/100 mit Min-Delta-Clamp",
              sc2(SYS_COSMO_RT_QUERY, HVST_SUB_DEADLINE_CONVERT, 0), 0);
}

static void test_hvst_register_present(void) {
    check_val("clock_event hv-stimer rating 400 registriert unter Hyper-V",
              sc2(SYS_COSMO_RT_QUERY, HVST_SUB_REGISTER_PRESENT, 0), 0);
}

static void test_hvst_shutdown(void) {
    check_val("stimer shutdown idempotent (skip ohne Hyper-V)",
              sc2(SYS_COSMO_RT_QUERY, HVST_SUB_SHUTDOWN, 0), 0);
}

TEST("hv_stimer/init_no_hyperv",   test_hvst_init_no_hyperv);
TEST("hv_stimer/config_build",     test_hvst_config_build);
TEST("hv_stimer/deadline_convert", test_hvst_deadline_convert);
TEST("hv_stimer/register_present", test_hvst_register_present);
TEST("hv_stimer/shutdown",         test_hvst_shutdown);
