/* Calibrated timer — uses TSC, calibrated against HPET (preferred) or PIT.
 * Also reads CMOS RTC at boot for wall-clock epoch offset. */

#include "core/timer.h"
#include "core/clocksource.h"
#include "arch/hpet.h"
#include "hw/serial.h"
#include "arch/arch.h"

static inline uint64_t rdtsc(void) { return arch_rdtsc(); }

/* CPUID: AMD/Intel Extended Power Management. EDX bit 8 = Invariant TSC. */
#define CPUID_LEAF_POWER_MGMT    0x80000007u
#define CPUID_INVARIANT_TSC_BIT  (1u << 8)
#define CPUID_LEAF_EXT_MAX       0x80000000u

#define PIT_FREQ                 1193182u
#define PIT_10MS_TICKS           (PIT_FREQ / 100)   /* 11932 ticks */
#define PIT_CH2_CTRL_PORT        0x43
#define PIT_CH2_DATA_PORT        0x42
#define PIT_GATE_PORT            0x61
#define PIT_CH2_MODE_LOHI_MODE0  0xB0
#define PIT_GATE_ENABLE          0x01
#define PIT_GATE_SPEAKER_MASK    0xFC
#define PIT_OUT_BIT              0x20

#define TSC_HPET_MIN_TICKS       10000u             /* ~100 us @ 100 MHz */
#define TSC_HPET_TIMEOUT_TSC     50000000ULL        /* ~20 ms @ 2.5 GHz */
#define TSC_NS_PER_MS            1000000ULL
#define TSC_DEFAULT_HZ_FALLBACK  1000000000ULL
#define TSC_MULT_MAX_SEC_WINDOW  600u

static uint64_t tsc_clocksource_read(void) { return arch_rdtsc(); }

static struct clocksource tsc_clocksource = {
    .name  = "tsc",
    .rating = CLOCKSOURCE_RATING_TSC,
    .read  = tsc_clocksource_read,
    .mask  = CLOCKSOURCE_MASK_64,
    .flags = CLOCKSOURCE_FLAG_CONTINUOUS | CLOCKSOURCE_FLAG_VALID_FOR_HRES,
};

uint64_t timer_tsc_per_ms = 0;
uint64_t timer_boot_tsc = 0;
#define boot_tsc timer_boot_tsc

static int      tsc_invariant_state;          /* 0 = unknown/not, 1 = invariant */
static int      tsc_invariant_override = -1;  /* -1 auto, 0 fake-not, 1 fake-yes */
static int      tsc_via_hpet;
static uint64_t tsc_hz;

static void serial_dec(uint64_t v) {
    char t[24]; int i = 0;
    do { t[i++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (i--) serial_putchar(t[i]);
}

__attribute__((cold))
static int tsc_check_invariant(void) {
    if (tsc_invariant_override >= 0) return tsc_invariant_override;
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    arch_cpuid(CPUID_LEAF_EXT_MAX, &eax, &ebx, &ecx, &edx);
    if (eax < CPUID_LEAF_POWER_MGMT) return 0;
    arch_cpuid(CPUID_LEAF_POWER_MGMT, &eax, &ebx, &ecx, &edx);
    return (edx & CPUID_INVARIANT_TSC_BIT) ? 1 : 0;
}

/* HPET-basierte TSC-Kalibrierung: baseline lesen, mindestens TSC_HPET_MIN_TICKS
 * HPET-Ticks abwarten, dann TSC- und HPET-Delta messen. Liefert TSC-Frequenz
 * in Hz oder 0 bei Timeout / HPET unverfuegbar. */
__attribute__((cold))
static uint64_t calibrate_tsc_via_hpet(void) {
    if (!hpet_available()) return 0;
    uint64_t hpet_hz = hpet_frequency_hz();
    if (hpet_hz == 0) return 0;

    uint64_t mask = hpet_counter_is_64bit() ? CLOCKSOURCE_MASK_64
                                            : CLOCKSOURCE_MASK_32;

    uint64_t hpet_start = hpet_read() & mask;
    uint64_t tsc_start  = rdtsc();

    uint64_t hpet_now = hpet_start;
    uint64_t hpet_delta = 0;
    while (hpet_delta < TSC_HPET_MIN_TICKS) {
        if ((rdtsc() - tsc_start) > TSC_HPET_TIMEOUT_TSC) return 0;
        hpet_now = hpet_read() & mask;
        hpet_delta = (hpet_now - hpet_start) & mask;
    }

    uint64_t tsc_end = rdtsc();
    uint64_t tsc_delta = tsc_end - tsc_start;

    return (tsc_delta * hpet_hz) / hpet_delta;
}

__attribute__((cold))
static uint64_t calibrate_tsc_via_pit(void) {
    arch_outb(PIT_CH2_CTRL_PORT, PIT_CH2_MODE_LOHI_MODE0);
    arch_outb(PIT_CH2_DATA_PORT, (uint8_t)(PIT_10MS_TICKS & 0xFF));
    arch_outb(PIT_CH2_DATA_PORT, (uint8_t)(PIT_10MS_TICKS >> 8));

    uint8_t gate = arch_inb(PIT_GATE_PORT);
    gate = (uint8_t)((gate & PIT_GATE_SPEAKER_MASK) | PIT_GATE_ENABLE);
    arch_outb(PIT_GATE_PORT, gate);
    gate &= (uint8_t)~PIT_GATE_ENABLE;
    arch_outb(PIT_GATE_PORT, gate);
    gate |= PIT_GATE_ENABLE;
    arch_outb(PIT_GATE_PORT, gate);

    uint64_t tsc_start = rdtsc();
    uint8_t status;
    do {
        status = arch_inb(PIT_GATE_PORT);
    } while (!(status & PIT_OUT_BIT));
    uint64_t tsc_end = rdtsc();

    uint64_t tsc_10ms = tsc_end - tsc_start;
    return tsc_10ms * 100ULL;
}

__attribute__((cold))
static void tsc_calibrate_and_register(void) {
    uint64_t hz = calibrate_tsc_via_hpet();
    tsc_via_hpet = hz != 0;
    if (!hz) hz = calibrate_tsc_via_pit();

    tsc_hz = hz;
    timer_tsc_per_ms = hz / 1000ULL;
    if (timer_tsc_per_ms == 0) {
        timer_tsc_per_ms = TSC_DEFAULT_HZ_FALLBACK / 1000ULL;
        tsc_hz           = TSC_DEFAULT_HZ_FALLBACK;
    }

    tsc_invariant_state = tsc_check_invariant();
    tsc_clocksource.rating = tsc_invariant_state ? CLOCKSOURCE_RATING_TSC
                                                 : CLOCKSOURCE_RATING_TSC_UNSTABLE;

    clocksource_hz_to_mult(&tsc_clocksource, tsc_hz, TSC_MULT_MAX_SEC_WINDOW);

    serial_puts("tsc: calibrated via ");
    serial_puts(tsc_via_hpet ? "hpet" : "pit");
    serial_puts(", freq=");
    serial_dec(tsc_hz / 1000000ULL);
    serial_puts("MHz, invariant=");
    serial_puts(tsc_invariant_state ? "yes" : "no");
    serial_puts("\n");
}

__attribute__((cold))
void timer_init(void) {
    boot_tsc = rdtsc();
    tsc_calibrate_and_register();
    clocksource_register(&tsc_clocksource);
}

int      tsc_is_invariant(void)        { return tsc_invariant_state; }
int      tsc_calibrated_via_hpet(void) { return tsc_via_hpet; }
uint64_t tsc_calibrated_hz(void)       { return tsc_hz; }

__attribute__((cold))
void tsc_override_invariant_for_test(int state) {
    tsc_invariant_override = state;
}

__attribute__((cold))
void tsc_recalibrate_for_test(void) {
    clocksource_unregister(&tsc_clocksource);
    tsc_calibrate_and_register();
    clocksource_register(&tsc_clocksource);
}

__attribute__((cold))
uint64_t tsc_calibrate_via_hpet_for_test(void) {
    return calibrate_tsc_via_hpet();
}

uint64_t timer_ms(void) {
    if (timer_tsc_per_ms == 0) return 0;
    return (rdtsc() - boot_tsc) / timer_tsc_per_ms;
}

uint32_t timer_epoch_sec(void) {
    return (uint32_t)(rtc_epoch_sec + timer_ms() / 1000);
}

/* Kernel-only non-preemptible delay. For HW init timing (SMP SIPI, device
 * reset). NOT for userspace sleep — use event_wait with timeout instead. */
void timer_sleep_ms(uint32_t ms) {
    if (ms < 10) {
        uint64_t target = rdtsc() + (uint64_t)ms * timer_tsc_per_ms;
        while (rdtsc() < target)
            arch_pause();
    } else {
        uint64_t deadline = timer_ms() + ms;
        while (timer_ms() < deadline)
            arch_halt();
    }
}

/* ── CMOS RTC → Unix epoch ────────────────────── */

uint64_t rtc_epoch_sec = 0;

static uint8_t cmos_read(uint8_t reg) {
    arch_outb(0x70, reg);
    return arch_inb(0x71);
}

static uint8_t bcd2bin(uint8_t v) { return (v & 0x0F) + (v >> 4) * 10; }

/* Days from 1970-01-01 to year/month/day (Gauss algorithm).
 * month: 1-12, day: 1-31. Handles leap years correctly. */
static uint64_t days_since_epoch(int y, int m, int d) {
    if (m <= 2) { y--; m += 12; }
    m -= 3;
    uint64_t era = (uint64_t)y / 400;
    int yoe = y - (int)(era * 400);
    int doy = (153 * m + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (uint64_t)doe - 719468;
}

void rtc_init(void) {
    while (cmos_read(0x0A) & 0x80)
        ;

    uint8_t sec  = cmos_read(0x00);
    uint8_t min  = cmos_read(0x02);
    uint8_t hour = cmos_read(0x04);
    uint8_t day  = cmos_read(0x07);
    uint8_t mon  = cmos_read(0x08);
    uint8_t year = cmos_read(0x09);
    uint8_t regB = cmos_read(0x0B);

    if (!(regB & 0x04)) {
        sec  = bcd2bin(sec);
        min  = bcd2bin(min);
        hour = bcd2bin(hour);
        day  = bcd2bin(day);
        mon  = bcd2bin(mon);
        year = bcd2bin(year);
    }

    if (!(regB & 0x02) && (hour & 0x80)) {
        hour = ((hour & 0x7F) % 12) + 12;
    }

    int full_year = 2000 + (int)year;
    uint64_t days = days_since_epoch(full_year, (int)mon, (int)day);
    rtc_epoch_sec = days * 86400 + (uint64_t)hour * 3600
                  + (uint64_t)min * 60 + (uint64_t)sec;

    serial_puts("RTC: ");
    char tmp[20]; int ti = 0;
    uint64_t v = rtc_epoch_sec;
    do { tmp[ti++] = '0' + v % 10; v /= 10; } while (v);
    while (ti--) serial_putchar(tmp[ti]);
    serial_puts(" epoch sec\n");
}
