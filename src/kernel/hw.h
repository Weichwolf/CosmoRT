/* CosmoRT Hardware Primitives — the ENTIRE hardware abstraction
 *
 * 5 functions. Kernel maps, userspace accesses. No per-register syscall.
 *
 * cosmo_mmio_map     — map device registers into caller's address space
 * cosmo_dma_alloc    — allocate physically contiguous DMA-capable memory
 * cosmo_irq_register — register interrupt handler for an IRQ line
 * cosmo_pci_config_read — read PCI configuration space
 * cosmo_fw_load      — load firmware blob by name
 */
#ifndef HW_H
#define HW_H

#include <stdint.h>
#include <stddef.h>

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

/* Initialize hardware subsystem. Called once at boot. */
void hw_init(void);

#endif
