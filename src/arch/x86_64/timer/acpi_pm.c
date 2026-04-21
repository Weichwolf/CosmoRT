/* ACPI PM_TMR clocksource — I/O-Port-Counter bei 3.579545 MHz.
 *
 * Fallback falls HPET/TSC/pvclock nicht verfuegbar. Wraps je nach
 * FADT-Flag TMR_VAL_EXT: 24 bit (~4.69 s) oder 32 bit (~20 min). Die
 * clocksource-Core-Maske faengt den Wrap ab.
 *
 * QEMU: PIIX4-PM legt das Register typischerweise auf Port 0x608, Bochs/
 * Q35 ebenfalls. Kein MMIO-Fallback — wenn die FADT SystemMemory angibt,
 * wird die Zeitquelle verworfen (Linux macht das genauso bei 32-bit-Kerneln
 * ohne XPM_TMR). */

#include "arch/acpi_pm.h"
#include "arch/arch.h"
#include "core/clocksource.h"
#include "hw/acpi.h"
#include "hw/serial.h"

#define APM_RATING                 CLOCKSOURCE_RATING_ACPI_PM
#define APM_FADT_REV_HAS_FLAGS     2u       /* FADT.revision >= 2 garantiert flags */

static uint16_t           apm_port;
static int                apm_is_32bit;
static int                apm_registered;
static int                apm_test_override;
static volatile uint32_t *apm_test_counter;

static uint16_t           apm_saved_port;
static int                apm_saved_is_32bit;
static uint64_t           apm_saved_mask;
static uint32_t           apm_saved_mult, apm_saved_shift;
static uint32_t           apm_saved_flags;

static uint64_t acpi_pm_cs_read(void);

static struct clocksource acpi_pm_clocksource = {
    .name   = "acpi_pm",
    .rating = APM_RATING,
    .read   = acpi_pm_cs_read,
    .mask   = ACPI_PM_MASK_24BIT,
    .mult   = 0,
    .shift  = 0,
    .flags  = CLOCKSOURCE_FLAG_VALID_FOR_HRES,
};

static inline uint32_t acpi_pm_read_raw(void) {
    if (apm_test_override) return *apm_test_counter;
    return arch_inl(apm_port);
}

uint32_t acpi_pm_read(void) {
    if (!apm_port) return 0;
    uint32_t v = acpi_pm_read_raw();
    return apm_is_32bit ? v : (uint32_t)(v & ACPI_PM_MASK_24BIT);
}

static uint64_t acpi_pm_cs_read(void) {
    return (uint64_t)acpi_pm_read();
}

static void apm_serial_dec(uint64_t v) {
    char t[24]; int i = 0;
    do { t[i++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (i--) serial_putchar(t[i]);
}

static void apm_serial_hex(uint64_t v) {
    char t[16]; int i = 0;
    serial_puts("0x");
    do { uint8_t n = (uint8_t)(v & 0xF); t[i++] = (char)(n < 10 ? '0'+n : 'a'+n-10); v >>= 4; } while (v);
    while (i--) serial_putchar(t[i]);
}

static void acpi_pm_log(uint16_t port, int is_32bit) {
    serial_puts("acpi_pm: port=");
    apm_serial_hex(port);
    serial_puts(" width=");
    apm_serial_dec(is_32bit ? 32 : 24);
    serial_puts("-bit freq=");
    apm_serial_dec((uint64_t)ACPI_PM_FREQ_HZ / 1000u);
    serial_puts("kHz\n");
}

static void apm_configure(uint16_t port, int is_32bit) {
    apm_port     = port;
    apm_is_32bit = is_32bit;

    acpi_pm_clocksource.mask  = is_32bit ? ACPI_PM_MASK_32BIT : ACPI_PM_MASK_24BIT;
    acpi_pm_clocksource.flags = CLOCKSOURCE_FLAG_VALID_FOR_HRES;
    clocksource_hz_to_mult(&acpi_pm_clocksource,
                           ACPI_PM_FREQ_HZ, ACPI_PM_MAX_SEC_WINDOW);
}

__attribute__((cold))
void acpi_pm_init(void) {
    if (apm_test_override) return;

    apm_port     = 0;
    apm_is_32bit = 0;

    const struct acpi_sdt_header *h = acpi_find_table(ACPI_SIG_FADT);
    if (!h) {
        serial_puts("acpi_pm: no FADT\n");
        return;
    }

    const struct acpi_fadt *f = (const struct acpi_fadt *)h;
    if (f->pm_timer_block == 0 || f->pm_timer_block > 0xffffu) {
        serial_puts("acpi_pm: no PM_TMR block\n");
        return;
    }

    int is_32bit = 0;
    if (f->pm_timer_length == ACPI_PM_LEN_32BIT &&
        f->header.revision >= APM_FADT_REV_HAS_FLAGS &&
        (f->flags & ACPI_FADT_FLAG_TMR_VAL_EXT)) {
        is_32bit = 1;
    }

    apm_configure((uint16_t)f->pm_timer_block, is_32bit);
    acpi_pm_log(apm_port, apm_is_32bit);
    if (!apm_registered) {
        clocksource_register(&acpi_pm_clocksource);
        apm_registered = 1;
    }
}

int      acpi_pm_available(void) { return apm_port != 0; }
uint16_t acpi_pm_port(void)      { return apm_port; }
int      acpi_pm_is_32bit(void)  { return apm_is_32bit; }

void acpi_pm_override_for_test(volatile uint32_t *counter_src,
                               uint16_t port, int is_32bit) {
    if (!apm_test_override) {
        apm_saved_port     = apm_port;
        apm_saved_is_32bit = apm_is_32bit;
        apm_saved_mask     = acpi_pm_clocksource.mask;
        apm_saved_mult     = acpi_pm_clocksource.mult;
        apm_saved_shift    = acpi_pm_clocksource.shift;
        apm_saved_flags    = acpi_pm_clocksource.flags;
    }

    apm_test_counter = counter_src;
    apm_test_override = 1;
    apm_configure(port, is_32bit);
}

void acpi_pm_clear_override_for_test(void) {
    if (!apm_test_override) return;

    apm_port     = apm_saved_port;
    apm_is_32bit = apm_saved_is_32bit;

    acpi_pm_clocksource.mask  = apm_saved_mask;
    acpi_pm_clocksource.mult  = apm_saved_mult;
    acpi_pm_clocksource.shift = apm_saved_shift;
    acpi_pm_clocksource.flags = apm_saved_flags;

    apm_test_counter  = 0;
    apm_test_override = 0;
}
