/* TSC-Invariant-Check + HPET/PIT-Kalibrierung.
 *
 * Subcases 110-115 via SYS_COSMO_RT_QUERY. Kernel-Helfer:
 *   - tsc_override_invariant_for_test(state) erzwingt CPUID-Bit
 *   - tsc_recalibrate_for_test() wiederholt Kalibrierung
 * HPET-Override (Subcase 112) nutzt hpet_override_base_for_test. */
#include "ktest.h"
#include "cosmort.h"

#define TSC_SUB_INVARIANT_QUERY    110
#define TSC_SUB_HPET_FREQ_MATCH    111
#define TSC_SUB_PIT_FALLBACK       112
#define TSC_SUB_RATING_DOWNGRADE   113
#define TSC_SUB_FREQ_PLAUSIBLE     114
#define TSC_SUB_PATH_CONSISTENT    115

static void test_tsc_invariant_query(void) {
    puts("\n[TSC]\n");
    check_val("cpuid-basierter invariant-check meldet 0 oder 1",
              sc2(SYS_COSMO_RT_QUERY, TSC_SUB_INVARIANT_QUERY, 0), 0);
}

static void test_tsc_hpet_calibration(void) {
    check_val("hpet-kalibrierung trifft tsc-freq innerhalb toleranz",
              sc2(SYS_COSMO_RT_QUERY, TSC_SUB_HPET_FREQ_MATCH, 0), 0);
}

static void test_tsc_pit_fallback(void) {
    check_val("ohne hpet greift pit-fallback",
              sc2(SYS_COSMO_RT_QUERY, TSC_SUB_PIT_FALLBACK, 0), 0);
}

static void test_tsc_rating_downgrade(void) {
    check_val("fake-nicht-invariant -> rating tsc_unstable",
              sc2(SYS_COSMO_RT_QUERY, TSC_SUB_RATING_DOWNGRADE, 0), 0);
}

static void test_tsc_freq_plausible(void) {
    check_val("tsc-frequenz im plausiblen bereich (500mhz-5ghz)",
              sc2(SYS_COSMO_RT_QUERY, TSC_SUB_FREQ_PLAUSIBLE, 0), 0);
}

static void test_tsc_path_consistent(void) {
    check_val("kalibrierungs-pfad stimmt mit hpet_available ueberein",
              sc2(SYS_COSMO_RT_QUERY, TSC_SUB_PATH_CONSISTENT, 0), 0);
}

TEST("tsc/invariant_query",    test_tsc_invariant_query);
TEST("tsc/hpet_calibration",   test_tsc_hpet_calibration);
TEST("tsc/pit_fallback",       test_tsc_pit_fallback);
TEST("tsc/rating_downgrade",   test_tsc_rating_downgrade);
TEST("tsc/freq_plausible",     test_tsc_freq_plausible);
TEST("tsc/path_consistent",    test_tsc_path_consistent);
