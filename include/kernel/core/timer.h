/* Calibrated timer — geschwindigkeitsunabhaengige Zeitbasis */
#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

void timer_init(void);

uint64_t timer_ms(void);

void timer_sleep_ms(uint32_t ms);

uint32_t timer_epoch_sec(void);

extern uint64_t timer_tsc_per_ms;

extern uint64_t timer_boot_tsc;

static inline uint64_t timer_tsc_now(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t timer_deadline_tsc(uint64_t ms_from_now) {
    return timer_tsc_now() + ms_from_now * timer_tsc_per_ms;
}

extern uint64_t rtc_epoch_sec;

void rtc_init(void);

#endif
