#include "ktest.h"

void test_pci(void) {
    puts("\n[PCI Scan]\n");

    int found = 0;
    for (int bus = 0; bus < 8; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id = 0;
            long r = sc5(SYS_COSMO_PCI_READ, bus, dev, 0, 0, (long)&id);
            if (r < 0 || id == 0 || id == 0xFFFFFFFF) continue;
            uint32_t vendor = id & 0xFFFF;
            uint32_t device = (id >> 16) & 0xFFFF;
            puts("  PCI "); put_int(bus); puts(":"); put_int(dev);
            puts(".0 = "); put_hex(vendor); puts(":"); put_hex(device);

            /* Identify known devices */
            if (vendor == 0x8086 && (device == 0x100E || device == 0x100F))
                puts(" (E1000 NIC)");
            else if (vendor == 0x8086 && device == 0x1237)
                puts(" (440FX Host)");
            else if (vendor == 0x8086 && device == 0x7000)
                puts(" (PIIX3 ISA)");
            else if (vendor == 0x8086 && device == 0x7010)
                puts(" (PIIX3 IDE)");
            else if (vendor == 0x8086 && device == 0x7113)
                puts(" (PIIX4 ACPI)");
            else if (vendor == 0x1234 && device == 0x1111)
                puts(" (QEMU VGA)");
            else if (vendor == 0x1AF4)
                puts(" (virtio)");

            puts("\n");
            found++;
        }
    }
    check_ge("PCI devices found", (long)found, 1);
}
