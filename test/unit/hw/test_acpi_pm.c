/* ACPI PM_TMR clocksource self-tests.
 *
 * Subcases 80-84 via SYS_COSMO_RT_QUERY. Kernel-side override leitet die
 * acpi_pm_read-Funktion auf einen in-BSS uint32_t um, Port-IO bleibt in
 * den Tests unberuehrt. Subcase 84 (boot probe) ist bedingt: QEMU
 * PIIX4-PM / Q35 exponiert das Register immer; Hosts ohne PM_TMR lassen
 * den Treiber inert und der Test degradiert zu no-op. */
#include "ktest.h"
#include "cosmort.h"

#define ACPI_PM_SUB_MISSING_FADT     80
#define ACPI_PM_SUB_MASK_24_VS_32    81
#define ACPI_PM_SUB_MULT_SHIFT       82
#define ACPI_PM_SUB_MONOTONIC        83
#define ACPI_PM_SUB_BOOT_PROBE       84

static void test_acpi_pm_missing_fadt(void) {
    puts("\n[ACPI-PM]\n");
    check_val("acpi_pm_init no-op when FADT absent",
              sc2(SYS_COSMO_RT_QUERY, ACPI_PM_SUB_MISSING_FADT, 0), 0);
}

static void test_acpi_pm_mask(void) {
    check_val("24-bit vs 32-bit counter masking",
              sc2(SYS_COSMO_RT_QUERY, ACPI_PM_SUB_MASK_24_VS_32, 0), 0);
}

static void test_acpi_pm_mult_shift(void) {
    check_val("mult/shift fuer 3.579545 MHz innerhalb 100 ppm",
              sc2(SYS_COSMO_RT_QUERY, ACPI_PM_SUB_MULT_SHIFT, 0), 0);
}

static void test_acpi_pm_monotonic(void) {
    check_val("acpi_pm_read monoton bei steigendem Counter",
              sc2(SYS_COSMO_RT_QUERY, ACPI_PM_SUB_MONOTONIC, 0), 0);
}

static void test_acpi_pm_boot_probe(void) {
    check_val("acpi_pm_init registered clocksource when FADT PM_TMR present",
              sc2(SYS_COSMO_RT_QUERY, ACPI_PM_SUB_BOOT_PROBE, 0), 0);
}

TEST("acpi_pm/missing_fadt",   test_acpi_pm_missing_fadt);
TEST("acpi_pm/mask_24_vs_32",  test_acpi_pm_mask);
TEST("acpi_pm/mult_shift",     test_acpi_pm_mult_shift);
TEST("acpi_pm/monotonic",      test_acpi_pm_monotonic);
TEST("acpi_pm/boot_probe",     test_acpi_pm_boot_probe);
