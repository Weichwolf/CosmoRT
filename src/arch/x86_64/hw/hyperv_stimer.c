/* Hyper-V Synthetic Timer (STIMER0) als clock_event_device rating 400.
 *
 * Setzt oberhalb der bereits initialisierten TSC-Page (hyperv_tsc_read_raw)
 * und des SynIC auf. STIMER-COUNT ist absolute Zeit in 100ns-Einheiten
 * relativ zur selben Uhr — ein set_next_event(delta_ns) ergibt damit
 * trivial now_ticks + delta_ns/100. Fire-Pfad: bei erreichter Deadline
 * feuert der Hypervisor entweder einen IDT-Vektor (Direct-Mode,
 * TLFS v6.0b §15.3.5) oder eine SynIC-Message auf einem dedizierten SINT
 * (Legacy-Pfad).
 *
 * Direct-Mode bevorzugt, weil er einen Round-Trip durch den SynIC-Message-
 * Slot spart und der ISR-Pfad derselbe bleibt wie für alle anderen IRQs.
 * Verfügbarkeit via CPUID.0x40000003.EDX[19]. Fallback: SINT4 → 0x34 via
 * bestehende hyperv_synic_init()-Infrastruktur.
 *
 * Min-Deadline-Guard: Wenn COUNT <= now_ticks gesetzt würde, feuert der
 * STimer sofort und nach jedem ACK erneut — Interrupt-Storm. Deshalb klemmt
 * hyperv_stimer_compute_deadline() jede Deadline auf mindestens
 * now + HV_STIMER_MIN_DELTA_TICKS (100us).
 */

#include "hw/hyperv.h"
#include "core/clocksource.h"
#include "core/irq.h"
#include "hw/serial.h"
#include "arch/arch.h"

#define HV_STIMER_CE_NAME         "hv-stimer"
#define HV_STIMER_CE_RATING       CLOCKSOURCE_RATING_HYPERV_TSC
#define HV_STIMER_SINT_AUTOEOI    (1ULL << 17)

static int  stimer_direct_mode;
static int  stimer_registered;

static uint64_t stimer_build_base(int periodic) {
    uint64_t cfg = HV_STIMER_CONFIG_ENABLE | HV_STIMER_CONFIG_AUTO_ENABLE;
    if (periodic) cfg |= HV_STIMER_CONFIG_PERIODIC;
    return cfg;
}

uint64_t hyperv_stimer_build_config_direct(uint8_t vector, int periodic) {
    uint64_t cfg = stimer_build_base(periodic);
    cfg |= HV_STIMER_CONFIG_DIRECT_MODE;
    cfg |= ((uint64_t)vector << HV_STIMER_CONFIG_VECTOR_SHIFT)
         & HV_STIMER_CONFIG_VECTOR_MASK;
    return cfg;
}

uint64_t hyperv_stimer_build_config_synic(uint8_t sint, int periodic) {
    uint64_t cfg = stimer_build_base(periodic);
    cfg |= ((uint64_t)(sint & 0xF) << HV_STIMER_CONFIG_SINTX_SHIFT)
         & HV_STIMER_CONFIG_SINTX_MASK;
    return cfg;
}

uint64_t hyperv_stimer_compute_deadline(uint64_t now_ticks, uint64_t delta_ns) {
    uint64_t delta_ticks = delta_ns / HV_STIMER_TICK_NS;
    if (delta_ticks < HV_STIMER_MIN_DELTA_TICKS)
        delta_ticks = HV_STIMER_MIN_DELTA_TICKS;
    return now_ticks + delta_ticks;
}

int hyperv_stimer_direct_mode_available(void) {
    if (!hyperv_detect()) return 0;
    uint32_t eax, ebx, ecx, edx;
    arch_cpuid(HV_CPUID_FEATURES, &eax, &ebx, &ecx, &edx);
    return (edx & (1u << HV_FEATURE_STIMER_DIRECT_BIT)) ? 1 : 0;
}

int hyperv_stimer_available(void) {
    if (!hyperv_detect()) return 0;
    if (!hyperv_tsc_page()) return 0;
    uint32_t eax, ebx, ecx, edx;
    arch_cpuid(HV_CPUID_FEATURES, &eax, &ebx, &ecx, &edx);
    return (eax & (1u << HV_FEATURE_STIMER_MSRS_BIT)) ? 1 : 0;
}

static void stimer_program(uint64_t delta_ns) {
    uint64_t now = hyperv_tsc_read_raw();
    uint64_t deadline = hyperv_stimer_compute_deadline(now, delta_ns);

    uint64_t cfg = stimer_direct_mode
        ? hyperv_stimer_build_config_direct(HV_STIMER0_DIRECT_VECTOR, 0)
        : hyperv_stimer_build_config_synic(HV_STIMER_SINT, 0);

    arch_wrmsr(HV_X64_MSR_STIMER0_COUNT, 0);      /* disarm before reprogram */
    arch_wrmsr(HV_X64_MSR_STIMER0_CONFIG, cfg);
    arch_wrmsr(HV_X64_MSR_STIMER0_COUNT, deadline);
}

static int stimer_set_next_event(uint64_t delta_ns) {
    stimer_program(delta_ns);
    return 0;
}

static int stimer_shutdown(void) {
    arch_wrmsr(HV_X64_MSR_STIMER0_COUNT, 0);
    arch_wrmsr(HV_X64_MSR_STIMER0_CONFIG, 0);
    return 0;
}

static void stimer_isr(int vector) {
    (void)vector;
    /* AUTO_ENABLE im CONFIG-MSR löscht Enable auf Delivery — der Timer ist
     * nach Fire automatisch disarmed. Re-Arming erfolgt durch den nächsten
     * set_next_event-Call aus dem Scheduler. EOI genügt hier. */
    lapic_eoi();
}

static struct clock_event_device hv_stimer_ce = {
    .name           = HV_STIMER_CE_NAME,
    .rating         = HV_STIMER_CE_RATING,
    .features       = CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_SHUTDOWN,
    .set_next_event = stimer_set_next_event,
    .set_periodic   = 0,
    .shutdown       = stimer_shutdown,
};

__attribute__((cold))
void hyperv_stimer_init(void) {
    if (stimer_registered) return;
    if (!hyperv_stimer_available()) return;

    stimer_direct_mode = hyperv_stimer_direct_mode_available();

    if (stimer_direct_mode) {
        irq_register(HV_STIMER0_DIRECT_VECTOR, stimer_isr);
    } else {
        arch_wrmsr(HV_X64_MSR_SINT0 + HV_STIMER_SINT,
                   (uint64_t)HV_STIMER_SINT_VECTOR | HV_STIMER_SINT_AUTOEOI);
        irq_register(HV_STIMER_SINT_VECTOR, stimer_isr);
    }

    clock_event_register(&hv_stimer_ce);
    stimer_registered = 1;

    serial_puts(stimer_direct_mode
                ? "hyperv: stimer0 registered (direct-mode, rating 400)\n"
                : "hyperv: stimer0 registered (synic SINT4, rating 400)\n");
}
