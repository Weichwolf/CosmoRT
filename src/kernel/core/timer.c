/* Calibrated timer — uses TSC, calibrated against APIC timer. */

#include "core/timer.h"
#include "core/irq.h"
#include "hw/serial.h"
#include "arch/arch.h"

static int lapic_ready = 0;

static inline uint64_t rdtsc(void) { return arch_rdtsc(); }

uint64_t timer_tsc_per_ms = 0;
uint64_t timer_boot_tsc = 0;
#define boot_tsc timer_boot_tsc

void timer_init(void) {
    boot_tsc = rdtsc();

    #define PIT_FREQ 1193182
    #define PIT_10MS (PIT_FREQ / 100)

    arch_outb(0x43, 0xB0);
    arch_outb(0x42, (uint8_t)(PIT_10MS & 0xFF));
    arch_outb(0x42, (uint8_t)(PIT_10MS >> 8));

    uint8_t gate = arch_inb(0x61);
    gate = (gate & 0xFC) | 0x01;
    arch_outb(0x61, gate);

    gate &= 0xFE;
    arch_outb(0x61, gate);
    gate |= 0x01;
    arch_outb(0x61, gate);

    uint64_t tsc_start = rdtsc();

    uint8_t status;
    do {
        status = arch_inb(0x61);
    } while (!(status & 0x20));

    uint64_t tsc_end = rdtsc();
    uint64_t tsc_10ms = tsc_end - tsc_start;

    timer_tsc_per_ms = tsc_10ms / 10;
    if (timer_tsc_per_ms == 0) timer_tsc_per_ms = 1000000;

    serial_puts("Timer: ");
    char tmp[20]; int ti = 0;
    uint64_t v = timer_tsc_per_ms;
    do { tmp[ti++] = '0' + v % 10; v /= 10; } while (v);
    while (ti--) serial_putchar(tmp[ti]);
    serial_puts(" TSC/ms\n");

    lapic_ready = 1;
}

uint64_t timer_ms(void) {
    if (timer_tsc_per_ms == 0) return 0;
    return (rdtsc() - boot_tsc) / timer_tsc_per_ms;
}

uint32_t timer_epoch_sec(void) {
    return (uint32_t)(rtc_epoch_sec + timer_ms() / 1000);
}

void timer_sleep_ms(uint32_t ms) {
    if (ms == 0) return;

    if (lapic_ready) {
        lapic_delay_ms(ms);
    } else {
        uint64_t target = rdtsc() + (uint64_t)ms * timer_tsc_per_ms;
        while (rdtsc() < target)
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
    char tmp[20]; int ti = 0;
    uint64_t v = rtc_epoch_sec;
    do { tmp[ti++] = '0' + v % 10; v /= 10; } while (v);
    while (ti--) serial_putchar(tmp[ti]);
    serial_puts(" epoch sec\n");
}
