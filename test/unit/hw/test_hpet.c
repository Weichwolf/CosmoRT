/* HPET clocksource self-tests.
 *
 * Subcases 40-44 via SYS_COSMO_RT_QUERY. Kernel-side helpers redirect the
 * driver's MMIO window to an in-BSS buffer via hpet_override_base_for_test.
 * Subcase 44 (boot probe) is conditional: on QEMU -machine q35 HPET is
 * exposed via ACPI and verifies the real init path; on hosts without HPET
 * the driver is inert and the test degrades to a no-op success. */
#include "ktest.h"
#include "cosmort.h"

#define HPET_SUB_PERIOD_FREQ      40
#define HPET_SUB_READ_COUNTER     41
#define HPET_SUB_CLOCKSOURCE_MASK 42
#define HPET_SUB_MULT_SHIFT       43
#define HPET_SUB_BOOT_PROBE       44

static void test_hpet_period_freq(void) {
    puts("\n[HPET]\n");
    check_val("caps decode yields period + freq + width",
              sc2(SYS_COSMO_RT_QUERY, HPET_SUB_PERIOD_FREQ, 0), 0);
}

static void test_hpet_read_counter(void) {
    check_val("hpet_read returns main counter MMIO word",
              sc2(SYS_COSMO_RT_QUERY, HPET_SUB_READ_COUNTER, 0), 0);
}

static void test_hpet_clocksource_mask(void) {
    check_val("32-bit vs 64-bit counter maps to width flag",
              sc2(SYS_COSMO_RT_QUERY, HPET_SUB_CLOCKSOURCE_MASK, 0), 0);
}

static void test_hpet_mult_shift(void) {
    check_val("mult/shift from femtosec period within 100 ppm",
              sc2(SYS_COSMO_RT_QUERY, HPET_SUB_MULT_SHIFT, 0), 0);
}

static void test_hpet_boot_probe(void) {
    check_val("hpet_init registered clocksource when ACPI HPET present",
              sc2(SYS_COSMO_RT_QUERY, HPET_SUB_BOOT_PROBE, 0), 0);
}

TEST("hpet/period_freq",      test_hpet_period_freq);
TEST("hpet/read_counter",     test_hpet_read_counter);
TEST("hpet/clocksource_mask", test_hpet_clocksource_mask);
TEST("hpet/mult_shift",       test_hpet_mult_shift);
TEST("hpet/boot_probe",       test_hpet_boot_probe);
