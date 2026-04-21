/* HPET clocksource — MMIO Main Counter als monotone Zeitquelle.
 *
 * Layout folgt IA-PC HPET 1.0a. Alle Register-Offsets / Bitmasken in
 * include/kernel/arch/hpet.h. Zugriff 64-bit via volatile Pointer in den
 * Direct-Map (PHYS_OFFSET + phys).
 *
 * Registrierung: clocksource rating 250. TSC (300) bleibt primary solange
 * verfuegbar — HPET ist Fallback und dient spaeter als Kalibrations-Quelle
 * fuer TSC (Phase 7.7.9). */

#include "arch/hpet.h"
#include "core/clocksource.h"
#include "hw/acpi.h"
#include "hw/serial.h"
#include "mm/paging.h"
#include "config.h"

#define HPET_MMIO_REGION_BYTES  0x1000u
#define HPET_MAX_SEC_WINDOW     600u
#define HPET_RATING             CLOCKSOURCE_RATING_HPET

static volatile uint64_t *hpet_mmio;
static uint64_t           hpet_phys_base;
static uint32_t           hpet_period;          /* femtoseconds per tick */
static uint64_t           hpet_hz;
static int                hpet_is_64bit;
static int                hpet_comparators;
static int                hpet_test_override;

static volatile uint64_t *hpet_saved_mmio;
static uint64_t           hpet_saved_phys_base;
static uint32_t           hpet_saved_period;
static uint64_t           hpet_saved_hz;
static int                hpet_saved_is_64bit;
static int                hpet_saved_comparators;
static uint64_t           hpet_saved_mask;
static uint32_t           hpet_saved_mult, hpet_saved_shift;

static struct clocksource hpet_clocksource = {
    .name   = "hpet",
    .rating = HPET_RATING,
    .read   = 0,
    .mask   = 0,
    .mult   = 0,
    .shift  = 0,
    .flags  = CLOCKSOURCE_FLAG_CONTINUOUS | CLOCKSOURCE_FLAG_VALID_FOR_HRES,
};

static inline uint64_t hpet_mmio_read(uint32_t off) {
    return *(volatile uint64_t *)((uintptr_t)hpet_mmio + off);
}

static inline void hpet_mmio_write(uint32_t off, uint64_t val) {
    *(volatile uint64_t *)((uintptr_t)hpet_mmio + off) = val;
}

static uint64_t hpet_cs_read(void) {
    return hpet_mmio_read(HPET_REG_MAIN_CNT);
}

static void hpet_serial_dec(uint64_t v) {
    char t[24]; int i = 0;
    do { t[i++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (i--) serial_putchar(t[i]);
}

static void hpet_serial_hex(uint64_t v) {
    char t[16]; int i = 0;
    serial_puts("0x");
    do { uint8_t n = (uint8_t)(v & 0xF); t[i++] = (char)(n < 10 ? '0'+n : 'a'+n-10); v >>= 4; } while (v);
    while (i--) serial_putchar(t[i]);
}

static void hpet_log_found(uint64_t phys, uint32_t period, uint64_t hz, int bits) {
    serial_puts("hpet: base=");
    hpet_serial_hex(phys);
    serial_puts(" period=");
    hpet_serial_dec(period);
    serial_puts("fs freq=");
    hpet_serial_dec(hz / 1000000ULL);
    serial_puts("MHz width=");
    hpet_serial_dec((uint64_t)bits);
    serial_puts("bit\n");
}

static int hpet_configure_from_caps(uint64_t caps) {
    hpet_period       = (uint32_t)(caps >> HPET_GCAP_PERIOD_SHIFT);
    hpet_is_64bit     = (caps & HPET_GCAP_COUNT_SIZE) ? 1 : 0;
    hpet_comparators  = (int)((caps >> HPET_GCAP_NUM_TIM_SHIFT) & HPET_GCAP_NUM_TIM_MASK) + 1;

    if (hpet_period < HPET_PERIOD_MIN_FS || hpet_period > HPET_PERIOD_MAX_FS)
        return -1;

    hpet_hz = HPET_FEMTO_PER_SEC / (uint64_t)hpet_period;

    hpet_clocksource.read = hpet_cs_read;
    hpet_clocksource.mask = hpet_is_64bit ? CLOCKSOURCE_MASK_64 : CLOCKSOURCE_MASK_32;
    clocksource_hz_to_mult(&hpet_clocksource, hpet_hz, HPET_MAX_SEC_WINDOW);
    return 0;
}

static void hpet_enable_counter(void) {
    for (int n = 0; n < hpet_comparators; n++) {
        uint64_t cfg = hpet_mmio_read(HPET_REG_TN_CONF(n));
        cfg &= ~(uint64_t)HPET_TN_INT_ENB;
        hpet_mmio_write(HPET_REG_TN_CONF(n), cfg);
    }

    hpet_mmio_write(HPET_REG_MAIN_CNT, 0);

    uint64_t gconf = hpet_mmio_read(HPET_REG_GCONF);
    gconf &= ~(uint64_t)HPET_GCONF_LEG_RT;
    gconf |= HPET_GCONF_ENABLE;
    hpet_mmio_write(HPET_REG_GCONF, gconf);
}

__attribute__((cold))
void hpet_init(void) {
    if (hpet_test_override) return;

    hpet_mmio        = 0;
    hpet_phys_base   = 0;
    hpet_period      = 0;
    hpet_hz          = 0;
    hpet_is_64bit    = 0;
    hpet_comparators = 0;

    const struct acpi_sdt_header *h = acpi_find_table(ACPI_SIG_HPET);
    if (!h) {
        serial_puts("hpet: no ACPI table\n");
        return;
    }

    const struct acpi_hpet *ht = (const struct acpi_hpet *)h;
    if (ht->base_address.address_space_id != 0 || ht->base_address.address == 0) {
        serial_puts("hpet: unsupported address space\n");
        return;
    }

    hpet_phys_base = ht->base_address.address;
    paging_map_2mb(hpet_phys_base);
    hpet_mmio = (volatile uint64_t *)phys_to_virt(hpet_phys_base);

    uint64_t caps = hpet_mmio_read(HPET_REG_GCAP_ID);
    if (hpet_configure_from_caps(caps) != 0) {
        serial_puts("hpet: invalid period\n");
        hpet_mmio      = 0;
        hpet_phys_base = 0;
        return;
    }

    hpet_enable_counter();

    hpet_log_found(hpet_phys_base, hpet_period, hpet_hz, hpet_is_64bit ? 64 : 32);
    clocksource_register(&hpet_clocksource);
}

uint64_t hpet_read(void) {
    if (!hpet_mmio) return 0;
    return hpet_mmio_read(HPET_REG_MAIN_CNT);
}

int      hpet_available(void)        { return hpet_mmio != 0; }
uint64_t hpet_base_phys(void)        { return hpet_phys_base; }
uint32_t hpet_period_fs(void)        { return hpet_period; }
uint64_t hpet_frequency_hz(void)     { return hpet_hz; }
int      hpet_num_comparators(void)  { return hpet_comparators; }
int      hpet_counter_is_64bit(void) { return hpet_is_64bit; }

void hpet_override_base_for_test(uint64_t virt, uint32_t period_fs,
                                 int is_64bit, int num_comp) {
    if (!hpet_test_override) {
        hpet_saved_mmio        = hpet_mmio;
        hpet_saved_phys_base   = hpet_phys_base;
        hpet_saved_period      = hpet_period;
        hpet_saved_hz          = hpet_hz;
        hpet_saved_is_64bit    = hpet_is_64bit;
        hpet_saved_comparators = hpet_comparators;
        hpet_saved_mask        = hpet_clocksource.mask;
        hpet_saved_mult        = hpet_clocksource.mult;
        hpet_saved_shift       = hpet_clocksource.shift;
    }

    hpet_mmio        = (volatile uint64_t *)(uintptr_t)virt;
    hpet_phys_base   = (virt >= PHYS_OFFSET) ? virt - PHYS_OFFSET : 0;
    hpet_period      = period_fs;
    hpet_hz          = period_fs ? (HPET_FEMTO_PER_SEC / (uint64_t)period_fs) : 0;
    hpet_is_64bit    = is_64bit;
    hpet_comparators = num_comp;

    hpet_clocksource.read = hpet_cs_read;
    hpet_clocksource.mask = is_64bit ? CLOCKSOURCE_MASK_64 : CLOCKSOURCE_MASK_32;
    if (hpet_hz)
        clocksource_hz_to_mult(&hpet_clocksource, hpet_hz, HPET_MAX_SEC_WINDOW);
    hpet_test_override = 1;
}

void hpet_clear_override_for_test(void) {
    if (!hpet_test_override) return;
    hpet_mmio        = hpet_saved_mmio;
    hpet_phys_base   = hpet_saved_phys_base;
    hpet_period      = hpet_saved_period;
    hpet_hz          = hpet_saved_hz;
    hpet_is_64bit    = hpet_saved_is_64bit;
    hpet_comparators = hpet_saved_comparators;

    hpet_clocksource.mask  = hpet_saved_mask;
    hpet_clocksource.mult  = hpet_saved_mult;
    hpet_clocksource.shift = hpet_saved_shift;
    hpet_test_override = 0;
}
