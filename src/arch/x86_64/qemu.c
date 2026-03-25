/* CosmoRT QEMU Platform Support — shutdown, reboot, detection */

#include "arch.h"
#include "serial.h"

/* Try multiple ACPI shutdown methods.
 * QEMU i440FX: PM1a_CNT at 0x604
 * QEMU Q35:    PM1a_CNT at 0x404
 * Bochs/OVMF:  PM1a_CNT at 0xB004 */
void arch_shutdown(void) {
    /* S5 sleep type = 0x2000 (SLP_TYP=5 << 10 | SLP_EN=1 << 13) */
    __asm__ volatile("outw %0, %1" :: "a"((unsigned short)0x2000), "Nd"((unsigned short)0x604));
    __asm__ volatile("outw %0, %1" :: "a"((unsigned short)0x2000), "Nd"((unsigned short)0x404));
    __asm__ volatile("outw %0, %1" :: "a"((unsigned short)0x2000), "Nd"((unsigned short)0xB004));
    serial_puts("arch_shutdown: ACPI failed\n");
    __asm__ volatile("cli; hlt");
}
