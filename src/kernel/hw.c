/* CosmoRT Hardware Primitives — 5 kernel functions for all hardware access
 *
 * MMIO:  map device registers via page tables (direct map or dedicated VA)
 * DMA:   allocate contiguous physical pages, return both virt + phys
 * IRQ:   route I/O APIC IRQ to a handler function
 * PCI:   x86 port I/O config space access (bus/dev/fn/reg)
 * FW:    load firmware from embedded storage (ramfs/initrd)
 */

#include "hw.h"
#include "config.h"
#include "page_alloc.h"
#include "paging.h"
#include "irq.h"
#include "serial.h"
#include "spinlock.h"

/* ── MMIO Mapping ────────────────────────────────── */

int cosmo_mmio_map(uint64_t phys, size_t len, void **virt) {
    if (!virt) return -1;

    /* Round to 2MB boundary for paging_map_2mb */
    uint64_t start = phys & ~0x1FFFFFULL;
    uint64_t end = (phys + len + 0x1FFFFF) & ~0x1FFFFFULL;

    for (uint64_t addr = start; addr < end; addr += 0x200000)
        paging_map_2mb(addr);

    /* Return direct-map address */
    *virt = (void *)(phys + PHYS_OFFSET);
    return 0;
}

/* ── DMA Allocation ──────────────────────────────── */

int cosmo_dma_alloc(size_t len, void **virt, uint64_t *phys) {
    if (!virt || !phys) return -1;

    int npages = (int)((len + 4095) / 4096);
    void *v = pages_alloc(npages);
    if (!v) return -1;

    *virt = v;
    *phys = virt_to_phys(v);
    return 0;
}

void cosmo_dma_free(void *virt, size_t len) {
    if (!virt) return;
    int npages = (int)((len + 4095) / 4096);
    pages_free(virt, npages);
}

/* ── IRQ Registration ────────────────────────────── */

#define HW_MAX_IRQ 24

static struct {
    void (*handler)(void *);
    void *ctx;
} irq_table[HW_MAX_IRQ];

static spinlock_t irq_table_lock = SPINLOCK_INIT;

/* Internal IRQ dispatcher — called from irq.c generic handler */
static void hw_irq_dispatch(int vector) {
    int irq = vector - 32;  /* APIC vectors start at 32 */
    if (irq >= 0 && irq < HW_MAX_IRQ && irq_table[irq].handler)
        irq_table[irq].handler(irq_table[irq].ctx);
}

int cosmo_irq_register(int irq, void (*handler)(void *), void *ctx) {
    if (irq < 0 || irq >= HW_MAX_IRQ || !handler) return -1;

    uint64_t flags;
    spin_lock_irq(&irq_table_lock, &flags);

    irq_table[irq].handler = handler;
    irq_table[irq].ctx = ctx;

    /* Route I/O APIC IRQ to vector 32+irq */
    int vector = 32 + irq;
    extern void ioapic_route_irq(uint8_t irq, uint8_t vector);
    ioapic_route_irq((uint8_t)irq, (uint8_t)vector);

    /* Register in kernel IRQ handler table */
    irq_register(vector, hw_irq_dispatch);

    spin_unlock_irq(&irq_table_lock, flags);

    serial_puts("hw: IRQ ");
    serial_putchar('0' + (irq / 10));
    serial_putchar('0' + (irq % 10));
    serial_puts(" → vector ");
    serial_putchar('0' + (vector / 10));
    serial_putchar('0' + (vector % 10));
    serial_putchar('\n');

    return 0;
}

/* ── PCI Configuration Space ─────────────────────── */

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %w1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile("inl %w1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static spinlock_t pci_lock = SPINLOCK_INIT;

int cosmo_pci_config_read(int bus, int dev, int fn, int reg, uint32_t *val) {
    if (!val) return -1;
    if (bus < 0 || bus > 255 || dev < 0 || dev > 31 ||
        fn < 0 || fn > 7 || (reg & 3)) return -1;

    uint32_t addr = 0x80000000
        | ((uint32_t)bus << 16)
        | ((uint32_t)dev << 11)
        | ((uint32_t)fn << 8)
        | ((uint32_t)reg & 0xFC);

    uint64_t flags;
    spin_lock_irq(&pci_lock, &flags);
    outl(PCI_CONFIG_ADDR, addr);
    *val = inl(PCI_CONFIG_DATA);
    spin_unlock_irq(&pci_lock, flags);

    return 0;
}

int cosmo_pci_config_write(int bus, int dev, int fn, int reg, uint32_t val) {
    if (bus < 0 || bus > 255 || dev < 0 || dev > 31 ||
        fn < 0 || fn > 7 || (reg & 3)) return -1;

    uint32_t addr = 0x80000000
        | ((uint32_t)bus << 16)
        | ((uint32_t)dev << 11)
        | ((uint32_t)fn << 8)
        | ((uint32_t)reg & 0xFC);

    uint64_t flags;
    spin_lock_irq(&pci_lock, &flags);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
    spin_unlock_irq(&pci_lock, flags);

    return 0;
}

/* ── Firmware Loading ────────────────────────────── */

int cosmo_fw_load(const char *name, void **data, size_t *len) {
    /* TODO: load from ramfs/initrd when VFS is available.
     * For now: no firmware blobs embedded. */
    (void)name;
    if (data) *data = 0;
    if (len) *len = 0;
    return -2; /* -ENOENT */
}

/* ── Init ────────────────────────────────────────── */

void hw_init(void) {
    for (int i = 0; i < HW_MAX_IRQ; i++) {
        irq_table[i].handler = 0;
        irq_table[i].ctx = 0;
    }
    serial_puts("hw: 5 primitives ready\n");
}
