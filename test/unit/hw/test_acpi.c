/* ACPI table parser self-tests.
 *
 * Each TEST invokes SYS_COSMO_RT_QUERY subcase 20-30. The kernel-side helper
 * builds fake RSDP/XSDT/RSDT/SDT layouts in kernel BSS, installs them via
 * acpi_override_rsdp_for_test(), exercises acpi_find_table / acpi_madt_count,
 * and restores real state before return. */
#include "ktest.h"
#include "cosmort.h"

#define ACPI_SUB_RSDP_SIG        20
#define ACPI_SUB_RSDP_CHECKSUM   21
#define ACPI_SUB_REVISION_ROUTE  22
#define ACPI_SUB_XSDT_FIND       23
#define ACPI_SUB_XSDT_MISSING    24
#define ACPI_SUB_FADT_PM_TMR     25
#define ACPI_SUB_HPET_FIELDS     26
#define ACPI_SUB_MADT_COUNTS     27
#define ACPI_SUB_CORRUPT_SDT     28
#define ACPI_SUB_DUPLICATE_SIG   29
#define ACPI_SUB_CHECKSUM_HELPER 30

static void test_acpi_rsdp_sig(void) {
    puts("\n[ACPI]\n");
    check_val("RSDP signature + rev2 checksum",
              sc2(SYS_COSMO_RT_QUERY, ACPI_SUB_RSDP_SIG, 0), 0);
}

static void test_acpi_rsdp_checksum(void) {
    check_val("RSDP rejects corrupt checksum",
              sc2(SYS_COSMO_RT_QUERY, ACPI_SUB_RSDP_CHECKSUM, 0), 0);
}

static void test_acpi_revision_route(void) {
    check_val("rev0 → RSDT, rev2 → XSDT",
              sc2(SYS_COSMO_RT_QUERY, ACPI_SUB_REVISION_ROUTE, 0), 0);
}

static void test_acpi_xsdt_find(void) {
    check_val("XSDT walk locates FADT/HPET/MCFG",
              sc2(SYS_COSMO_RT_QUERY, ACPI_SUB_XSDT_FIND, 0), 0);
}

static void test_acpi_xsdt_missing(void) {
    check_val("XSDT walk returns NULL on missing sig",
              sc2(SYS_COSMO_RT_QUERY, ACPI_SUB_XSDT_MISSING, 0), 0);
}

static void test_acpi_fadt_pm_tmr(void) {
    check_val("FADT PM_TMR fields readable",
              sc2(SYS_COSMO_RT_QUERY, ACPI_SUB_FADT_PM_TMR, 0), 0);
}

static void test_acpi_hpet_fields(void) {
    check_val("HPET base + min_tick readable",
              sc2(SYS_COSMO_RT_QUERY, ACPI_SUB_HPET_FIELDS, 0), 0);
}

static void test_acpi_madt_counts(void) {
    check_val("MADT counts enabled LAPIC + IOAPIC",
              sc2(SYS_COSMO_RT_QUERY, ACPI_SUB_MADT_COUNTS, 0), 0);
}

static void test_acpi_corrupt_sdt(void) {
    check_val("corrupt SDT rejected",
              sc2(SYS_COSMO_RT_QUERY, ACPI_SUB_CORRUPT_SDT, 0), 0);
}

static void test_acpi_duplicate_sig(void) {
    check_val("duplicate signature → first match wins",
              sc2(SYS_COSMO_RT_QUERY, ACPI_SUB_DUPLICATE_SIG, 0), 0);
}

static void test_acpi_checksum_helper(void) {
    check_val("acpi_checksum_ok detects flip",
              sc2(SYS_COSMO_RT_QUERY, ACPI_SUB_CHECKSUM_HELPER, 0), 0);
}

TEST("acpi/rsdp_signature",    test_acpi_rsdp_sig);
TEST("acpi/rsdp_checksum",     test_acpi_rsdp_checksum);
TEST("acpi/revision_routing",  test_acpi_revision_route);
TEST("acpi/xsdt_find",         test_acpi_xsdt_find);
TEST("acpi/xsdt_missing",      test_acpi_xsdt_missing);
TEST("acpi/fadt_pm_tmr",       test_acpi_fadt_pm_tmr);
TEST("acpi/hpet_fields",       test_acpi_hpet_fields);
TEST("acpi/madt_counts",       test_acpi_madt_counts);
TEST("acpi/corrupt_sdt",       test_acpi_corrupt_sdt);
TEST("acpi/duplicate_sig",     test_acpi_duplicate_sig);
TEST("acpi/checksum_helper",   test_acpi_checksum_helper);
