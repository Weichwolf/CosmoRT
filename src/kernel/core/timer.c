/* Calibrated timer — frequency-invariant time base via TSC + PIT. */

#include "core/timer.h"
#include "core/irq.h"
#include "hw/serial.h"
#include "arch/arch.h"

#define PIT_FREQ          1193182
#define PIT_10MS          (PIT_FREQ / 100)
#define PIT_CMD_PORT      0x43
#define PIT_CH2_PORT      0x42
#define PIT_GATE_PORT     0x61
#define PIT_GATE_ENABLE   0x01
#define PIT_GATE_MASK     0xFC
#define PIT_OUT_BIT       0x20
#define PIT_CH2_MODE      0xB0

#define CPUID_INVARIANT_TSC_LEAF 0x80000007
#define CPUID_INVARIANT_TSC_BIT  (1u << 8)
#define CPUID_EXT_MAX_LEAF       0x80000000

#define TSC_PER_MS_FALLBACK 1000000

static int lapic_ready = 0;

uint64_t timer_tsc_per_ms = 0;
uint64_t timer_boot_tsc = 0;
uint64_t tsc_khz = 0;
int tsc_invariant = 0;

#define boot_tsc timer_boot_tsc

static void serial_uint(uint64_t v) {
    char tmp[20]; int ti = 0;
    do { tmp[ti++] = '0' + v % 10; v /= 10; } while (v);
    while (ti--) serial_putchar(tmp[ti]);
}

static void tsc_check_invariant(void) {
    uint32_t eax, ebx, ecx, edx;
    arch_cpuid(CPUID_EXT_MAX_LEAF, &eax, &ebx, &ecx, &edx);
    if (eax < CPUID_INVARIANT_TSC_LEAF) {
        serial_puts("TSC: CPUID 0x80000007 not supported — assuming variant\n");
        return;
    }
    arch_cpuid(CPUID_INVARIANT_TSC_LEAF, &eax, &ebx, &ecx, &edx);
    if (edx & CPUID_INVARIANT_TSC_BIT) {
        tsc_invariant = 1;
        serial_puts("TSC: invariant\n");
    } else {
        serial_puts("TSC: WARNING — not invariant, timers may drift under P-state changes\n");
    }
}

void timer_init(void) {
    boot_tsc = arch_rdtsc();

    tsc_check_invariant();

    arch_outb(PIT_CMD_PORT, PIT_CH2_MODE);
    arch_outb(PIT_CH2_PORT, (uint8_t)(PIT_10MS & 0xFF));
    arch_outb(PIT_CH2_PORT, (uint8_t)(PIT_10MS >> 8));

    uint8_t gate = arch_inb(PIT_GATE_PORT);
    gate = (gate & PIT_GATE_MASK) | PIT_GATE_ENABLE;
    arch_outb(PIT_GATE_PORT, gate);

    gate &= ~PIT_GATE_ENABLE;
    arch_outb(PIT_GATE_PORT, gate);
    gate |= PIT_GATE_ENABLE;
    arch_outb(PIT_GATE_PORT, gate);

    uint64_t tsc_start = arch_rdtsc();

    uint8_t status;
    do {
        status = arch_inb(PIT_GATE_PORT);
    } while (!(status & PIT_OUT_BIT));

    uint64_t tsc_end = arch_rdtsc();
    uint64_t tsc_10ms = tsc_end - tsc_start;

    timer_tsc_per_ms = tsc_10ms / 10;
    if (timer_tsc_per_ms == 0) timer_tsc_per_ms = TSC_PER_MS_FALLBACK;

    tsc_khz = timer_tsc_per_ms;

    serial_puts("Timer: ");
    serial_uint(timer_tsc_per_ms);
    serial_puts(" TSC/ms (");
    serial_uint(tsc_khz);
    serial_puts(" kHz)\n");

    lapic_ready = 1;
}

uint64_t timer_ms(void) {
    if (timer_tsc_per_ms == 0) return 0;
    return (arch_rdtsc() - boot_tsc) / timer_tsc_per_ms;
}

uint32_t timer_epoch_sec(void) {
    return (uint32_t)(rtc_epoch_sec + timer_ms() / 1000);
}

void timer_sleep_ms(uint32_t ms) {
    if (ms == 0) return;

    if (lapic_ready) {
        lapic_delay_ms(ms);
    } else {
        uint64_t target = arch_rdtsc() + (uint64_t)ms * timer_tsc_per_ms;
        while (arch_rdtsc() < target)
            arch_pause();
    }
}

uint64_t rtc_epoch_sec = 0;

static uint8_t cmos_read(uint8_t reg) {
    arch_outb(0x70, reg);
    return arch_inb(0x71);
}

static uint8_t bcd2bin(uint8_t v) { return (v & 0x0F) + (v >> 4) * 10; }

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
    serial_uint(rtc_epoch_sec);
    serial_puts(" epoch sec\n");
}
