/* CosmoRT NOHZ_FULL — tickless operation on isolated cores */

#include "core/nohz.h"
#include "core/percpu.h"
#include "core/timer.h"
#include "config.h"

#define LAPIC_PHYS          0xFEE00000
#define LAPIC_BASE          (LAPIC_PHYS + PHYS_OFFSET)
#define LAPIC_TIMER_REG     0x320
#define LAPIC_TIMER_INIT    0x380
#define LAPIC_TIMER_DIV     0x3E0

#define LAPIC_LVT_MASKED    0x10000
#define LAPIC_LVT_PERIODIC  0x20020
#define LAPIC_LVT_ONESHOT   0x20
#define LAPIC_TICKS_PER_MS  100000
#define LAPIC_DIVIDER       0x03

static volatile uint32_t *lapic = (volatile uint32_t *)LAPIC_BASE;

static uint8_t tickless[SMP_MAX_CORES];

static inline void nohz_lapic_write(uint32_t reg, uint32_t val) {
    lapic[reg / 4] = val;
}

void nohz_enter_tickless(int cpu) {
    if (cpu == 0) return;
    if (cpu < 0 || cpu >= SMP_MAX_CORES) return;
    if (percpu_self()->core_id != cpu) return;

    tickless[cpu] = 1;
    nohz_lapic_write(LAPIC_TIMER_REG, LAPIC_LVT_MASKED);
    nohz_lapic_write(LAPIC_TIMER_INIT, 0);
}

void nohz_exit_tickless(int cpu) {
    if (cpu < 0 || cpu >= SMP_MAX_CORES) return;
    if (percpu_self()->core_id != cpu) return;

    tickless[cpu] = 0;
    nohz_lapic_write(LAPIC_TIMER_DIV, LAPIC_DIVIDER);
    nohz_lapic_write(LAPIC_TIMER_REG, LAPIC_LVT_PERIODIC);
    nohz_lapic_write(LAPIC_TIMER_INIT, LAPIC_TICKS_PER_MS);
}

int nohz_is_tickless(int cpu) {
    if (cpu < 0 || cpu >= SMP_MAX_CORES) return 0;
    return tickless[cpu];
}

void nohz_arm_oneshot(int cpu, uint64_t deadline_tsc) {
    if (cpu < 0 || cpu >= SMP_MAX_CORES) return;
    if (percpu_self()->core_id != cpu) return;
    if (!tickless[cpu]) return;

    uint64_t now = timer_tsc_now();
    if (deadline_tsc <= now) {
        nohz_lapic_write(LAPIC_TIMER_DIV, LAPIC_DIVIDER);
        nohz_lapic_write(LAPIC_TIMER_REG, LAPIC_LVT_ONESHOT);
        nohz_lapic_write(LAPIC_TIMER_INIT, 1);
        return;
    }

    uint64_t delta_tsc = deadline_tsc - now;
    uint64_t ticks = delta_tsc * LAPIC_TICKS_PER_MS / timer_tsc_per_ms;
    if (ticks == 0) ticks = 1;
    if (ticks > 0xFFFFFFFF) ticks = 0xFFFFFFFF;

    nohz_lapic_write(LAPIC_TIMER_DIV, LAPIC_DIVIDER);
    nohz_lapic_write(LAPIC_TIMER_REG, LAPIC_LVT_ONESHOT);
    nohz_lapic_write(LAPIC_TIMER_INIT, (uint32_t)ticks);
}

void nohz_cancel_oneshot(int cpu) {
    if (cpu < 0 || cpu >= SMP_MAX_CORES) return;
    if (percpu_self()->core_id != cpu) return;

    nohz_lapic_write(LAPIC_TIMER_INIT, 0);
}
