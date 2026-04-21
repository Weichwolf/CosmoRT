/* HPET (High Precision Event Timer) — x86_64 MMIO timer.
 *
 * Registriert sich als clocksource rating 250 (< TSC=300, > ACPI-PM=110).
 * clock_event via Comparator N ist in 7.7.3 nicht Teil des Scopes — optional
 * spaeter via hpet_arm_comparator(). */
#ifndef ARCH_HPET_H
#define ARCH_HPET_H

#include <stdint.h>

/* Spec: IA-PC HPET Specification 1.0a. */
#define HPET_REG_GCAP_ID        0x000   /* General Capabilities and ID */
#define HPET_REG_GCONF          0x010   /* General Configuration */
#define HPET_REG_GISR           0x020   /* General Interrupt Status */
#define HPET_REG_MAIN_CNT       0x0F0   /* Main Counter Value */
#define HPET_REG_TN_CONF(n)     (0x100u + (uint32_t)(n) * 0x20u)
#define HPET_REG_TN_CMP(n)      (0x108u + (uint32_t)(n) * 0x20u)
#define HPET_REG_TN_FSB(n)      (0x110u + (uint32_t)(n) * 0x20u)

#define HPET_GCAP_NUM_TIM_SHIFT 8
#define HPET_GCAP_NUM_TIM_MASK  0x1Fu
#define HPET_GCAP_COUNT_SIZE    (1u << 13)
#define HPET_GCAP_LEG_RT_CAP    (1u << 15)
#define HPET_GCAP_PERIOD_SHIFT  32            /* femtoseconds per tick */

#define HPET_GCONF_ENABLE       (1u << 0)
#define HPET_GCONF_LEG_RT       (1u << 1)

#define HPET_TN_INT_TYPE        (1u << 1)     /* 0=edge, 1=level */
#define HPET_TN_INT_ENB         (1u << 2)
#define HPET_TN_TYPE_PERIODIC   (1u << 3)
#define HPET_TN_PER_INT_CAP     (1u << 4)
#define HPET_TN_SIZE_64         (1u << 5)
#define HPET_TN_VAL_SET         (1u << 6)
#define HPET_TN_32MODE          (1u << 8)
#define HPET_TN_INT_ROUTE_SHIFT 9
#define HPET_TN_FSB_EN          (1u << 14)
#define HPET_TN_FSB_CAP         (1u << 15)

#define HPET_FEMTO_PER_SEC      1000000000000000ULL
#define HPET_FEMTO_PER_NANO     1000000ULL
#define HPET_PERIOD_MIN_FS      100000u       /* 10 MHz — Spec-Minimum */
#define HPET_PERIOD_MAX_FS      100000000u    /* 10 kHz — Spec-Maximum */

/* Kein Abort wenn ACPI HPET fehlt — TSC bleibt primary. */
void     hpet_init(void);

/* Rohzugriff (u64 auch bei 32-bit-Counter — caller maskt ggf. selbst). */
uint64_t hpet_read(void);

/* Probes fuer Selbsttests. NULL / 0 / false wenn HPET nicht verfuegbar. */
int      hpet_available(void);
uint64_t hpet_base_phys(void);
uint32_t hpet_period_fs(void);
uint64_t hpet_frequency_hz(void);
int      hpet_num_comparators(void);
int      hpet_counter_is_64bit(void);

/* Force an explicit MMIO base for unit tests. Passing 0 disables the test
 * override and re-scans ACPI at the next hpet_init() call. */
void     hpet_override_base_for_test(uint64_t virt, uint32_t period_fs,
                                     int is_64bit, int num_comp);
void     hpet_clear_override_for_test(void);

#endif
