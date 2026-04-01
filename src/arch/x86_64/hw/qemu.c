/* CosmoRT QEMU Platform Support — shutdown, reboot, detection */

#include "arch/arch.h"
#include "hw/serial.h"

/* Try multiple ACPI shutdown methods.
 * QEMU i440FX: PM1a_CNT at 0x604
 * QEMU Q35:    PM1a_CNT at 0x404
 * Bochs/OVMF:  PM1a_CNT at 0xB004 */
void arch_shutdown(void) {
    /* S5 sleep type = 0x2000 (SLP_TYP=5 << 10 | SLP_EN=1 << 13).
     * Try all known PM1a_CNT ports. QEMU exits on the right one.
     * Loop prevents serial_puts from racing before QEMU terminates. */
    __asm__ volatile("outw %0, %1" :: "a"((unsigned short)0x2000), "Nd"((unsigned short)0x604));
    __asm__ volatile("outw %0, %1" :: "a"((unsigned short)0x2000), "Nd"((unsigned short)0x404));
    __asm__ volatile("outw %0, %1" :: "a"((unsigned short)0x2000), "Nd"((unsigned short)0xB004));
    for (;;) __asm__ volatile("hlt");
}
