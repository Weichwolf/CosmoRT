/* Calibrated timer — geschwindigkeitsunabhaengige Zeitbasis */
#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/* Initialize: calibrate TSC against APIC timer */
void timer_init(void);

/* Milliseconds since boot */
uint64_t timer_ms(void);

/* Sleep for ms milliseconds (busy-wait with HLT) */
void timer_sleep_ms(uint32_t ms);

/* TSC ticks per millisecond (after calibration) */
extern uint64_t timer_tsc_per_ms;

/* Boot TSC value (for absolute TSC→ms conversion) */
extern uint64_t timer_boot_tsc;

/* Raw TSC read */
static inline uint64_t timer_tsc_now(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Absolute TSC deadline for now + ms_from_now */
static inline uint64_t timer_deadline_tsc(uint64_t ms_from_now) {
    return timer_tsc_now() + ms_from_now * timer_tsc_per_ms;
}

/* Seconds since Unix epoch at boot (from CMOS RTC) */
extern uint64_t rtc_epoch_sec;

/* Read CMOS RTC and set rtc_epoch_sec. Call once at boot. */
void rtc_init(void);

#endif
