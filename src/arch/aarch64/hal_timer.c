/* aarch64 HAL - Timer (stubs).
 *
 * Phase-9 placeholder. Real impl will use the Generic Timer: CNTV_CTL_EL0
 * for control, CNTVCT_EL0 for reads, CNTV_TVAL_EL0/CNTV_CVAL_EL0 for
 * one-shot deadlines.
 *
 * Not linked into the x86_64 build.
 */

#include "hal/hal_timer.h"
#include "kernel/panic.h"

#define AARCH64_STUB(name) \
    do { (void)0; panic("aarch64 hal_timer_" #name " not implemented"); } while (0)

void hal_timer_init(void)                          { AARCH64_STUB(init); }
void hal_timer_set_oneshot(uint64_t dns)           { (void)dns; AARCH64_STUB(set_oneshot); }
void hal_timer_set_periodic(uint64_t pns)          { (void)pns; AARCH64_STUB(set_periodic); }
void hal_timer_disarm(void)                        { AARCH64_STUB(disarm); }
uint64_t hal_timer_now_ns(void)                    { AARCH64_STUB(now_ns); return 0; }
uint64_t hal_timer_ticks_per_ms(void)              { AARCH64_STUB(ticks_per_ms); return 0; }
