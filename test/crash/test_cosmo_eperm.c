/* Verify non-driver processes get -EPERM on all CosmoRT HW syscalls.
 * Forked child (is_driver=0 after fork) must be denied. */
#include "ktest.h"
#include "cosmort.h"

static void test_cosmo_eperm(void) {
    puts("\n[cosmo EPERM]\n");

    check_val("kexec",        sc2(SYS_COSMO_KEXEC, 0, 0),        -EPERM);
    check_val("mmio_map",     sc3(SYS_COSMO_MMIO_MAP, 0, 0, 0),  -EPERM);
    check_val("dma_alloc",    sc3(SYS_COSMO_DMA_ALLOC, 0, 0, 0), -EPERM);
    check_val("dma_free",     sc2(SYS_COSMO_DMA_FREE, 0, 0),     -EPERM);
    check_val("irq_register", sc3(SYS_COSMO_IRQ_REGISTER, 0, 0, 0), -EPERM);
    check_val("pci_read",     sc5(SYS_COSMO_PCI_READ, 0, 0, 0, 0, 0), -EPERM);
    check_val("pci_write",    sc5(SYS_COSMO_PCI_WRITE, 0, 0, 0, 0, 0), -EPERM);
    check_val("fw_load",      sc3(SYS_COSMO_FW_LOAD, 0, 0, 0),  -EPERM);
    check_val("nic_attach",   sc1(SYS_COSMO_NIC_ATTACH, 0),      -EPERM);
}

CRASH_TEST("cosmo_eperm", test_cosmo_eperm);
