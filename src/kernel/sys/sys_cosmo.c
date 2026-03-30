/* CosmoRT — Hardware primitive syscalls (SYS_COSMO_*) for userspace drivers. */

#include "internal.h"
#include "core/rt.h"

#define HW_CAP_CHECK() do { \
    process_t *_p = proc_current(); \
    if (!_p || !_p->is_driver) return -EPERM; \
} while (0)

long do_cosmo_mmio_map(long a1, long a2, long a3) {
    HW_CAP_CHECK();
    void *virt;
    int r = cosmo_mmio_map((uint64_t)a1, (size_t)a2, &virt);
    if (r == 0) { r = copy_to_user((void *)a3, &virt, sizeof(virt)); if (r) return r; }
    return r;
}

long do_cosmo_dma_alloc(long a1, long a2, long a3) {
    HW_CAP_CHECK();
    void *virt; uint64_t phys;
    int r = cosmo_dma_alloc((size_t)a1, &virt, &phys);
    if (r == 0) {
        int r2 = copy_to_user((void *)a2, &virt, sizeof(virt)); if (r2) return r2;
        r2 = copy_to_user((void *)a3, &phys, sizeof(phys)); if (r2) return r2;
    }
    return r;
}

long do_cosmo_dma_free(long a1, long a2) {
    HW_CAP_CHECK();
    cosmo_dma_free((void *)a1, (size_t)a2);
    return 0;
}

long do_cosmo_irq_register(long a1, long a2, long a3) {
    HW_CAP_CHECK();
    if ((uint64_t)a2 >= 0x800000000000ULL || a2 == 0) return -EFAULT;
    return cosmo_irq_register((int)a1, (void (*)(void *))a2, (void *)a3);
}

long do_cosmo_pci_read(long a1, long a2, long a3, long a4, long a5) {
    HW_CAP_CHECK();
    if (!user_ok(a5, 4)) return -EFAULT;
    return cosmo_pci_config_read((int)a1, (int)a2, (int)a3, (int)a4, (uint32_t *)a5);
}

long do_cosmo_pci_write(long a1, long a2, long a3, long a4, long a5) {
    HW_CAP_CHECK();
    return cosmo_pci_config_write((int)a1, (int)a2, (int)a3, (int)a4, (uint32_t)a5);
}

long do_cosmo_fw_load(long a1, long a2, long a3) {
    HW_CAP_CHECK();
    if (!user_ok(a1, 1) || !user_ok(a2, 8) || !user_ok(a3, 8)) return -EFAULT;
    return cosmo_fw_load((const char *)a1, (void **)a2, (size_t *)a3);
}

long do_cosmo_nic_attach(long a1) {
    HW_CAP_CHECK();
    struct { uint64_t shm_phys; uint64_t shm_size; uint8_t mac[6]; } kargs;
    { int r = copy_from_user(&kargs, (const void *)a1, sizeof(kargs)); if (r) return r; }
    return net_port_attach(kargs.shm_phys, (size_t)kargs.shm_size, kargs.mac);
}

long do_cosmo_kexec(long a1, long a2) {
    HW_CAP_CHECK();
    if (!user_ok(a1, (size_t)a2)) return -EFAULT;
    extern int do_kexec(const void *, size_t);
    return do_kexec((const void *)a1, (size_t)a2);
}

long cosmo_dispatch(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a6;
    process_t *p = proc_current();
    if (!p || !p->is_driver) return -EPERM;
    switch (num) {
    case SYS_COSMO_MMIO_MAP:     return do_cosmo_mmio_map(a1, a2, a3);
    case SYS_COSMO_DMA_ALLOC:    return do_cosmo_dma_alloc(a1, a2, a3);
    case SYS_COSMO_DMA_FREE:     return do_cosmo_dma_free(a1, a2);
    case SYS_COSMO_IRQ_REGISTER: return do_cosmo_irq_register(a1, a2, a3);
    case SYS_COSMO_PCI_READ:     return do_cosmo_pci_read(a1, a2, a3, a4, a5);
    case SYS_COSMO_PCI_WRITE:    return do_cosmo_pci_write(a1, a2, a3, a4, a5);
    case SYS_COSMO_FW_LOAD:      return do_cosmo_fw_load(a1, a2, a3);
    case SYS_COSMO_NIC_ATTACH:   return do_cosmo_nic_attach(a1);
    case SYS_COSMO_KEXEC:        return do_cosmo_kexec(a1, a2);
    default:                     return -ENOSYS;
    }
}
