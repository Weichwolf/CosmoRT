/* Hyper-V Reference-TSC-Page als clocksource.
 *
 * Wrapper über die in hyperv_init() bereits gemappte TSC-Page. Liefert dem
 * Core einen Roh-Counter in 100ns-Einheiten (HV_TSC_TICK_NS), so dass die
 * Standard-mult/shift-Konversion (mult=100, shift=0) korrekte ns ergibt.
 *
 * Sequence-Lock-Pattern für Host-Side-Updates (Migration, Re-Calibration)
 * sitzt in hyperv_tsc_read_raw() — siehe TLFS v6.0b §12.7. Bei
 * sequence==0 liefert read() 0; der clocksource-Wrap-Mechanismus toleriert
 * das (delta == 0).
 *
 * Aktivierung nur unter Hyper-V (hyperv_detect()) und nur wenn die TSC-Page
 * tatsächlich allokiert wurde (hyperv_tsc_page()). Auf QEMU ohne
 * Hyper-V-Enlightenment ist hyperv_clocksource_init() ein no-op.
 */

#include "hw/hyperv.h"
#include "core/clocksource.h"
#include "hw/serial.h"

#define HV_CS_NAME              "hyperv_tsc"
#define HV_CS_RATING            CLOCKSOURCE_RATING_HYPERV_TSC
#define HV_CS_TICKS_PER_NS_MULT 100u
#define HV_CS_TICKS_PER_NS_SHIFT 0u

static uint64_t hv_cs_read(void) {
    return hyperv_tsc_read_raw();
}

static struct clocksource hyperv_tsc_cs = {
    .name   = HV_CS_NAME,
    .rating = HV_CS_RATING,
    .read   = hv_cs_read,
    .mask   = CLOCKSOURCE_MASK_64,
    .mult   = HV_CS_TICKS_PER_NS_MULT,
    .shift  = HV_CS_TICKS_PER_NS_SHIFT,
    .flags  = CLOCKSOURCE_FLAG_CONTINUOUS
            | CLOCKSOURCE_FLAG_VALID_FOR_HRES
            | CLOCKSOURCE_FLAG_SUSPEND_NONSTOP,
};

__attribute__((cold))
void hyperv_clocksource_init(void) {
    if (!hyperv_detect()) return;
    if (!hyperv_tsc_page()) return;

    clocksource_register(&hyperv_tsc_cs);
    serial_puts("hyperv: clocksource registered (rating 400)\n");
}
