/* CosmoRT Hardware Primitives — the ENTIRE hardware abstraction for drivers */
#ifndef COSMO_RT_H
#define COSMO_RT_H

#include <stdint.h>
#include <stddef.h>

#define COSMO_PHYS_OFFSET    0xFFFF800000000000ULL

#define SYS_COSMO_MMIO_MAP       0x10000
#define SYS_COSMO_DMA_ALLOC      0x10001
#define SYS_COSMO_DMA_FREE       0x10002
#define SYS_COSMO_IRQ_REGISTER   0x10003
#define SYS_COSMO_PCI_READ       0x10004
#define SYS_COSMO_PCI_WRITE      0x10005
#define SYS_COSMO_FW_LOAD        0x10006
#define SYS_COSMO_NIC_ATTACH     0x10007
#define SYS_COSMO_KEXEC          0x10008

#ifndef phys_to_virt
#define phys_to_virt(p) ((void *)((uint64_t)(p) + COSMO_PHYS_OFFSET))
#endif
#ifndef virt_to_phys
#define virt_to_phys(v) ((uint64_t)(v) - COSMO_PHYS_OFFSET)
#endif

int cosmo_mmio_map(uint64_t phys, size_t len, void **virt);

int cosmo_dma_alloc(size_t len, void **virt, uint64_t *phys);

void cosmo_dma_free(void *virt, size_t len);

int cosmo_irq_register(int irq, void (*handler)(void *), void *ctx);

int cosmo_pci_config_read(int bus, int dev, int fn, int reg, uint32_t *val);

int cosmo_pci_config_write(int bus, int dev, int fn, int reg, uint32_t val);

int cosmo_fw_load(const char *name, void **data, size_t *len);

void hw_allow_mmio(uint64_t phys, size_t len);

uint64_t hw_ms(void);

void hw_init(void);

typedef struct { volatile uint32_t next; volatile uint32_t owner; } hw_spinlock_t;
#define HW_SPINLOCK_INIT {0, 0}

void hw_spin_lock(hw_spinlock_t *l);
void hw_spin_unlock(hw_spinlock_t *l);
void hw_spin_lock_irq(hw_spinlock_t *l, uint64_t *flags);
void hw_spin_unlock_irq(hw_spinlock_t *l, uint64_t flags);

void hw_memcpy(void *dst, const void *src, size_t len);
void hw_memset(void *dst, int val, size_t len);

typedef struct {
    int  (*send)(const void *data, uint16_t len);
    int  (*recv)(void *buf, uint16_t bufsize);
    void (*get_mac)(uint8_t mac[6]);
    const char *name;
} nic_driver_t;

void net_nic_register(const nic_driver_t *nic);

typedef struct {
    uint16_t type;
    uint16_t code;
    int32_t  value;
} input_event_t;

typedef struct {
    const char *name;
} input_driver_t;

void input_register(const input_driver_t *drv);

void input_submit_event(const input_event_t *ev);

void serial_putchar(char c);
void serial_puts(const char *s);
void serial_hex64(uint64_t v);

#endif /* COSMO_RT_H */
