/* PCI device enumeration via /proc/bus/pci/devices */
#include "ktest.h"

static void test_pci(void) {
    puts("\n[PCI Scan]\n");

    char buf[2048];
    long fd = sc3(SYS_OPEN, (long)"/proc/bus/pci/devices", O_RDONLY, 0);
    check("open /proc/bus/pci/devices", fd >= 0);
    if (fd < 0) return;

    long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    sc1(SYS_CLOSE, fd);
    check("read pci devices", n > 0);
    if (n <= 0) return;
    buf[n] = 0;

    /* Count lines = device count */
    int found = 0;
    for (long i = 0; i < n; i++)
        if (buf[i] == '\n') found++;

    puts("  "); put_int(found); puts(" PCI devices\n");
    check_ge("PCI devices found", (long)found, 1);
}

TEST("pci", test_pci);
