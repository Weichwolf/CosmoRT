/* Calibrated timer — uses TSC, calibrated against APIC timer */

#include "timer.h"
#include "serial.h"

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint64_t timer_tsc_per_ms = 0;
static uint64_t boot_tsc = 0;

/* Calibrate TSC by measuring how many TSC ticks pass during a known
 * APIC timer interval. The APIC timer counts down from INIT value.
 * We use a short busy-wait calibration at boot. */
void timer_init(void) {
    boot_tsc = rdtsc();

    /* Use PIT (Programmable Interval Timer) channel 2 for calibration.
     * PIT runs at 1.193182 MHz. Count 11932 ticks = ~10ms. */
    #define PIT_FREQ 1193182
    #define PIT_10MS (PIT_FREQ / 100) /* 11932 ticks = 10ms */

    /* Setup PIT channel 2 for one-shot */
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0xB0), "Nd"((uint16_t)0x43)); /* ch2, lobyte/hibyte, mode 0 */
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)(PIT_10MS & 0xFF)), "Nd"((uint16_t)0x42));
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)(PIT_10MS >> 8)), "Nd"((uint16_t)0x42));

    /* Wait for PIT to count down by polling port 0x61 bit 5 */
    /* Enable PIT gate */
    uint8_t gate;
    __asm__ volatile("inb %1, %0" : "=a"(gate) : "Nd"((uint16_t)0x61));
    gate = (gate & 0xFC) | 0x01; /* enable gate, disable speaker */
    __asm__ volatile("outb %0, %1" : : "a"(gate), "Nd"((uint16_t)0x61));

    /* Reset flip-flop by writing to gate */
    gate &= 0xFE;
    __asm__ volatile("outb %0, %1" : : "a"(gate), "Nd"((uint16_t)0x61));
    gate |= 0x01;
    __asm__ volatile("outb %0, %1" : : "a"(gate), "Nd"((uint16_t)0x61));

    /* Measure TSC during PIT countdown */
    uint64_t tsc_start = rdtsc();

    /* Wait for PIT output (bit 5 of port 0x61 goes high) */
    uint8_t status;
    do {
        __asm__ volatile("inb %1, %0" : "=a"(status) : "Nd"((uint16_t)0x61));
    } while (!(status & 0x20));

    uint64_t tsc_end = rdtsc();
    uint64_t tsc_10ms = tsc_end - tsc_start;

    timer_tsc_per_ms = tsc_10ms / 10;
    if (timer_tsc_per_ms == 0) timer_tsc_per_ms = 1000000; /* fallback 1GHz */

    serial_puts("Timer: ");
    /* Print TSC/ms */
    char tmp[20]; int ti = 0;
    uint64_t v = timer_tsc_per_ms;
    do { tmp[ti++] = '0' + v % 10; v /= 10; } while (v);
    while (ti--) serial_putchar(tmp[ti]);
    serial_puts(" TSC/ms\n");
}

uint64_t timer_ms(void) {
    if (timer_tsc_per_ms == 0) return 0;
    return (rdtsc() - boot_tsc) / timer_tsc_per_ms;
}

void timer_sleep_ms(uint32_t ms) {
    uint64_t target = rdtsc() + (uint64_t)ms * timer_tsc_per_ms;
    while (rdtsc() < target)
        __asm__ volatile("pause");
}
