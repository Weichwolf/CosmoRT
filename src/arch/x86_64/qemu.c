/* CosmoRT QEMU Platform Support — shutdown, reboot, detection */

#include "arch/arch.h"
#include "hw/serial.h"

void arch_shutdown(void) {
    __asm__ volatile("outw %0, %1" :: "a"((unsigned short)0x2000), "Nd"((unsigned short)0x604));
    __asm__ volatile("outw %0, %1" :: "a"((unsigned short)0x2000), "Nd"((unsigned short)0x404));
    __asm__ volatile("outw %0, %1" :: "a"((unsigned short)0x2000), "Nd"((unsigned short)0xB004));
    for (;;) __asm__ volatile("hlt");
}
