/* ACPI Power Management Timer (PM_TMR) — Fallback-Clocksource.
 *
 * Fixed-frequency 3.579545 MHz Counter aus dem ACPI-Chipset. Lesbar als
 * 32-bit I/O-Port-Wort, effektive Breite 24 oder 32 bit je nach
 * FADT-Flag TMR_VAL_EXT. Registriert als clocksource rating 110 —
 * schlechter als HPET (250) / TSC (300) / KVM/Hyper-V (400), aber echter
 * Hardware-Counter wo alles andere fehlt. Keine IRQs, kein clock_event.
 * Vorbild: Linux drivers/clocksource/acpi_pm.c. */
#ifndef ARCH_ACPI_PM_H
#define ARCH_ACPI_PM_H

#include <stdint.h>

#define ACPI_PM_FREQ_HZ         3579545u          /* PMTMRFreq (ACPI 6.5 §4.8.3.3) */
#define ACPI_PM_MAX_SEC_WINDOW  600u              /* mult/shift-Fenster */
#define ACPI_PM_LEN_32BIT       4u                /* FADT pm_timer_length fuer 32-bit */
#define ACPI_PM_MASK_24BIT      0x00ffffffULL
#define ACPI_PM_MASK_32BIT      0xffffffffULL

void     acpi_pm_init(void);

uint32_t acpi_pm_read(void);
int      acpi_pm_available(void);
uint16_t acpi_pm_port(void);
int      acpi_pm_is_32bit(void);

/* Test hooks: redirect I/O reads to an in-kernel u32 so unit tests can
 * exercise masking + monotonic behaviour without touching real hardware.
 * Passing virt=NULL re-arms ACPI-driven init on next acpi_pm_init(). */
void     acpi_pm_override_for_test(volatile uint32_t *counter_src,
                                   uint16_t port, int is_32bit);
void     acpi_pm_clear_override_for_test(void);

#endif
