/* CosmoRT Hardware Primitives — the ENTIRE hardware abstraction for drivers
 *
 * This is the public driver API. Drivers in src/drivers/ include ONLY this
 * header (plus linux.h). No kernel internals (process.h, sched.h, etc.).
 *
 * 5 core primitives + DMA free + PCI write + driver-safe utilities.
 * All drivers are userspace. Kernel provides these primitives via syscall.
 */
#ifndef COSMO_RT_H
#define COSMO_RT_H

#include <stdint.h>
#include <stddef.h>

/* ══════════════════════════════════════════════════════════════════
 * Physical ↔ Virtual address translation (higher-half direct map)
 * ══════════════════════════════════════════════════════════════════ */

#define COSMO_PHYS_OFFSET    0xFFFF800000000000ULL

/* ══════════════════════════════════════════════════════════════════
 * Syscall numbers (for userspace drivers via syscall interface)
 * ══════════════════════════════════════════════════════════════════ */

#define SYS_COSMO_MMIO_MAP       0x10000
#define SYS_COSMO_DMA_ALLOC      0x10001
#define SYS_COSMO_DMA_FREE       0x10002
#define SYS_COSMO_IRQ_REGISTER   0x10003
#define SYS_COSMO_PCI_READ       0x10004
#define SYS_COSMO_PCI_WRITE      0x10005
#define SYS_COSMO_FW_LOAD        0x10006
#define SYS_COSMO_NIC_ATTACH     0x10007
#define SYS_COSMO_KEXEC          0x10008
#define SYS_COSMO_RT_QUERY       0x10009

#ifndef phys_to_virt
#define phys_to_virt(p) ((void *)((uint64_t)(p) + COSMO_PHYS_OFFSET))
#endif
#ifndef virt_to_phys
#define virt_to_phys(v) ((uint64_t)(v) - COSMO_PHYS_OFFSET)
#endif

/* ══════════════════════════════════════════════════════════════════
 * 5 Core Primitives
 * ══════════════════════════════════════════════════════════════════ */

/* Map MMIO region into kernel virtual address space.
 * phys: physical base address of the device registers.
 * len:  size in bytes (rounded up to page boundary).
 * virt: output — kernel-accessible virtual address.
 * Returns 0 on success, -1 on failure. */
int cosmo_mmio_map(uint64_t phys, size_t len, void **virt);

/* Allocate physically contiguous DMA-capable memory.
 * len:  size in bytes (rounded up to page boundary).
 * virt: output — kernel-accessible virtual address (zeroed).
 * phys: output — physical address (for device DMA descriptors).
 * Returns 0 on success, -1 on failure. */
int cosmo_dma_alloc(size_t len, void **virt, uint64_t *phys);

/* Free DMA memory previously allocated with cosmo_dma_alloc. */
void cosmo_dma_free(void *virt, size_t len);

/* Register an interrupt handler for an IRQ line.
 * irq:     IRQ number (I/O APIC input, typically 0-23).
 * handler: called in interrupt context with ctx argument.
 * ctx:     opaque context pointer passed to handler.
 * Returns 0 on success, -1 on failure. */
int cosmo_irq_register(int irq, void (*handler)(void *), void *ctx);

/* Read 32-bit value from PCI configuration space.
 * bus, dev, fn: PCI address (bus 0-255, device 0-31, function 0-7).
 * reg:          register offset (must be 4-byte aligned).
 * val:          output — 32-bit register value.
 * Returns 0 on success, -1 on failure. */
int cosmo_pci_config_read(int bus, int dev, int fn, int reg, uint32_t *val);

/* Write 32-bit value to PCI configuration space. */
int cosmo_pci_config_write(int bus, int dev, int fn, int reg, uint32_t val);

/* Load firmware blob by name.
 * name: firmware filename (e.g., "iwlwifi-8265.ucode").
 * data: output — pointer to firmware data (kernel memory, read-only).
 * len:  output — firmware size in bytes.
 * Returns 0 on success, -ENOENT if not found. */
int cosmo_fw_load(const char *name, void **data, size_t *len);

/* ══════════════════════════════════════════════════════════════════
 * MMIO range registration (kernel-side, called before drivers probe)
 * ══════════════════════════════════════════════════════════════════ */

/* Register a physical address range as a valid MMIO target.
 * Must be called before cosmo_mmio_map for that range. */
void hw_allow_mmio(uint64_t phys, size_t len);

/* ══════════════════════════════════════════════════════════════════
 * Driver-safe utilities (isolation boundary)
 * ══════════════════════════════════════════════════════════════════ */

/* Monotonic millisecond timestamp for driver timeouts. */
uint64_t hw_ms(void);

/* Initialize hardware subsystem. Called once at boot. */
void hw_init(void);

/* Drivers use these instead of kernel-internal spinlock.h / memops.h. */
typedef struct { volatile uint32_t next; volatile uint32_t owner; } hw_spinlock_t;
#define HW_SPINLOCK_INIT {0, 0}

void hw_spin_lock(hw_spinlock_t *l);
void hw_spin_unlock(hw_spinlock_t *l);
void hw_spin_lock_irq(hw_spinlock_t *l, uint64_t *flags);
void hw_spin_unlock_irq(hw_spinlock_t *l, uint64_t flags);

void hw_memcpy(void *dst, const void *src, size_t len);
void hw_memset(void *dst, int val, size_t len);

/* ══════════════════════════════════════════════════════════════════
 * NIC driver interface — register with network stack
 * ══════════════════════════════════════════════════════════════════ */

typedef struct {
    int  (*send)(const void *data, uint16_t len);
    int  (*recv)(void *buf, uint16_t bufsize);
    void (*get_mac)(uint8_t mac[6]);
    const char *name;  /* "e1000", "virtio-net", etc. */
} nic_driver_t;

/* Register a NIC driver. Called by driver init (e.g., e1000_init). */
void net_nic_register(const nic_driver_t *nic);

/* ══════════════════════════════════════════════════════════════════
 * Input driver interface — register with input subsystem
 * ══════════════════════════════════════════════════════════════════ */

/* Input event (matches Linux input_event layout) */
typedef struct {
    uint16_t type;   /* EV_KEY=1, EV_REL=2, EV_ABS=3 */
    uint16_t code;   /* KEY_A, REL_X, etc. */
    int32_t  value;  /* 1=press, 0=release, axis value */
} input_event_t;

/* Input driver interface — any keyboard/mouse driver implements this */
typedef struct {
    const char *name;
    /* Driver calls input_submit_event, no poll needed */
} input_driver_t;

void input_register(const input_driver_t *drv);

/* Called BY drivers when they receive an event */
void input_submit_event(const input_event_t *ev);

/* ══════════════════════════════════════════════════════════════════
 * Serial output — debug logging for drivers
 * ══════════════════════════════════════════════════════════════════ */

void serial_putchar(char c);
void serial_puts(const char *s);
void serial_hex64(uint64_t v);

#endif /* COSMO_RT_H */
