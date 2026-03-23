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

/* Seconds since Unix epoch at boot (from CMOS RTC) */
extern uint64_t rtc_epoch_sec;

/* Read CMOS RTC and set rtc_epoch_sec. Call once at boot. */
void rtc_init(void);

#endif
