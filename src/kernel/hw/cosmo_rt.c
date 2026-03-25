/* CosmoRT Hardware Primitives — 5 kernel functions for all hardware access
 *
 * MMIO:  map device registers via page tables (direct map or dedicated VA)
 * DMA:   allocate contiguous physical pages, return both virt + phys
 * IRQ:   route I/O APIC IRQ to a handler function
 * PCI:   x86 port I/O config space access (bus/dev/fn/reg)
 * FW:    load firmware from embedded storage (ramfs/initrd)
 */

#include "cosmo_rt.h"
#include "linux/abi.h"
#include "config.h"
#include "page_alloc.h"
#include "paging.h"
#include "irq.h"
#include "process.h"
#include "vma.h"
#include "serial.h"
#include "spinlock.h"
#include "memops.h"
#include "arch_x86.h"

/* ── MMIO Mapping ────────────────────────────────── */

#define MAX_MMIO_RANGES 32

static struct { uint64_t phys; size_t len; } allowed_mmio[MAX_MMIO_RANGES];
static int mmio_count;

void hw_allow_mmio(uint64_t phys, size_t len) {
    if (mmio_count < MAX_MMIO_RANGES) {
        allowed_mmio[mmio_count].phys = phys;
        allowed_mmio[mmio_count].len = len;
        mmio_count++;
    }
}

int cosmo_mmio_map(uint64_t phys, size_t len, void **virt) {
    if (!virt) return -1;

    /* Always allow LAPIC and IOAPIC */
    if (phys != 0xFEE00000ULL && phys != 0xFEC00000ULL) {
        int found = 0;
        for (int i = 0; i < mmio_count; i++) {
            if (phys >= allowed_mmio[i].phys &&
                phys + len <= allowed_mmio[i].phys + allowed_mmio[i].len) {
                found = 1;
                break;
            }
        }
        if (!found) return -1;
    }

    /* Map into user address space if called from a userspace driver */
    process_t *p = proc_current();
    if (p && p->is_driver) {
        /* Map MMIO pages into process's user address space */
        uint64_t user_va = p->mmap_next;
        size_t aligned_len = (len + 4095) & ~4095ULL;
        for (uint64_t off = 0; off < aligned_len; off += 4096) {
            /* Map device-memory page: RW, no-cache (PTE_PCD|PTE_PWT) */
            extern int map_user_page(uint64_t *pml4, uint64_t va, uint64_t pa, int prot);
            map_user_page(p->pml4, user_va + off, phys + off,
                          PROT_READ | PROT_WRITE);
        }
        /* Mark pages as uncacheable by setting PCD in PTEs */
        /* TODO: set PTE_PCD for MMIO pages */
        vma_insert(&p->vma_root, user_va, user_va + aligned_len,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE);
        p->mmap_next = user_va + aligned_len;
        *virt = (void *)user_va;
        return 0;
    }

    /* Kernel-only path: ensure 2MB mapping in direct map */
    uint64_t start = phys & ~0x1FFFFFULL;
    uint64_t end = (phys + len + 0x1FFFFF) & ~0x1FFFFFULL;
    for (uint64_t addr = start; addr < end; addr += 0x200000)
        paging_map_2mb(addr);
    *virt = (void *)(phys + PHYS_OFFSET);
    return 0;
}

/* ── DMA Allocation ──────────────────────────────── */

int cosmo_dma_alloc(size_t len, void **virt, uint64_t *phys) {
    if (!virt || !phys) return -1;

    int npages = (int)((len + 4095) / 4096);
    void *v = pages_alloc(npages);
    if (!v) return -1;

    uint64_t pa = virt_to_phys(v);
    *phys = pa;

    /* Map into user address space if called from a userspace driver */
    process_t *p = proc_current();
    if (p && p->is_driver) {
        uint64_t user_va = p->mmap_next;
        size_t aligned_len = (size_t)npages * 4096;
        for (int i = 0; i < npages; i++) {
            extern int map_user_page(uint64_t *pml4, uint64_t va, uint64_t pa, int prot);
            map_user_page(p->pml4, user_va + (uint64_t)i * 4096,
                          pa + (uint64_t)i * 4096, PROT_READ | PROT_WRITE);
        }
        vma_insert(&p->vma_root, user_va, user_va + aligned_len,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE);
        p->mmap_next = user_va + aligned_len;
        *virt = (void *)user_va;
    } else {
        *virt = v;
    }
    return 0;
}

void cosmo_dma_free(void *virt, size_t len) {
    if (!virt) return;
    int npages = (int)((len + 4095) / 4096);
    pages_free(virt, npages);
}

/* ── IRQ Registration (shared IRQ support) ───────── */

#define HW_MAX_IRQ 24
#define HW_MAX_HANDLERS_PER_IRQ 4

static struct {
    void (*handler)(void *);
    void *ctx;
} irq_table[HW_MAX_IRQ][HW_MAX_HANDLERS_PER_IRQ];

static spinlock_t irq_table_lock = SPINLOCK_INIT;

/* Internal IRQ dispatcher — calls ALL registered handlers for this IRQ */
static void hw_irq_dispatch(int vector) {
    int irq = vector - 32;  /* APIC vectors start at 32 */
    if (irq < 0 || irq >= HW_MAX_IRQ) return;
    for (int i = 0; i < HW_MAX_HANDLERS_PER_IRQ; i++) {
        if (irq_table[irq][i].handler)
            irq_table[irq][i].handler(irq_table[irq][i].ctx);
    }
}

int cosmo_irq_register(int irq, void (*handler)(void *), void *ctx) {
    if (irq < 0 || irq >= HW_MAX_IRQ || !handler) return -1;

    uint64_t flags;
    spin_lock_irq(&irq_table_lock, &flags);

    /* Find free slot for this IRQ */
    int slot = -1;
    for (int i = 0; i < HW_MAX_HANDLERS_PER_IRQ; i++) {
        if (!irq_table[irq][i].handler) { slot = i; break; }
    }
    if (slot < 0) {
        spin_unlock_irq(&irq_table_lock, flags);
        serial_puts("hw: IRQ table full for IRQ\n");
        return -1;
    }

    irq_table[irq][slot].handler = handler;
    irq_table[irq][slot].ctx = ctx;

    /* Route I/O APIC IRQ to vector 32+irq.
     * PCI uses level-triggered, active-low interrupts.
     * Set bit 13 (active-low) + bit 15 (level-triggered). */
    int vector = 32 + irq;
    extern void ioapic_route_irq_level(uint8_t irq, uint8_t vector);
    ioapic_route_irq_level((uint8_t)irq, (uint8_t)vector);

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

static inline void outl(uint16_t port, uint32_t val) { arch_outl(port, val); }
static inline uint32_t inl(uint16_t port) { return arch_inl(port); }

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

    /* Auto-register MMIO BARs (offset 0x10-0x24) as allowed MMIO ranges.
     * BAR bit 0 = 0 means memory-mapped (not I/O port). */
    if (reg >= 0x10 && reg <= 0x24 && !(*val & 1)) {
        uint64_t bar_phys = *val & 0xFFFFFFF0ULL;
        if (bar_phys)
            hw_allow_mmio(bar_phys, 0x200000);  /* conservative 2MB */
    }

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
    if (!name || !data || !len) return -EINVAL;

    /* Build path: /lib/firmware/<name> */
    char path[320];
    const char *prefix = "/lib/firmware/";
    int pi = 0;
    while (*prefix && pi < 300) path[pi++] = *prefix++;
    while (*name && pi < 318) path[pi++] = *name++;
    path[pi] = '\0';

    extern int vfs_read_file(const char *, uint8_t **, size_t *);
    uint8_t *fwdata = 0;
    size_t fwlen = 0;
    int r = vfs_read_file(path, &fwdata, &fwlen);
    if (r < 0) {
        *data = 0;
        *len = 0;
        return -ENOENT;
    }
    *data = fwdata;
    *len = fwlen;
    return 0;
}

/* ── Monotonic Time ──────────────────────────────── */

extern uint64_t timer_ms(void);

uint64_t hw_ms(void) {
    return timer_ms();
}

/* ── Init ────────────────────────────────────────── */

void hw_init(void) {
    for (int i = 0; i < HW_MAX_IRQ; i++)
        for (int j = 0; j < HW_MAX_HANDLERS_PER_IRQ; j++) {
            irq_table[i][j].handler = 0;
            irq_table[i][j].ctx = 0;
        }
    serial_puts("hw: 5 primitives ready\n");
}

/* ── Driver-safe wrappers ─────────────────────────── */

void hw_spin_lock(hw_spinlock_t *l)   { spin_lock((spinlock_t *)l); }
void hw_spin_unlock(hw_spinlock_t *l) { spin_unlock((spinlock_t *)l); }

void hw_spin_lock_irq(hw_spinlock_t *l, uint64_t *flags) {
    spin_lock_irq((spinlock_t *)l, flags);
}

void hw_spin_unlock_irq(hw_spinlock_t *l, uint64_t flags) {
    spin_unlock_irq((spinlock_t *)l, flags);
}

void hw_memcpy(void *dst, const void *src, size_t len) { kmemcpy(dst, src, len); }
void hw_memset(void *dst, int val, size_t len) { kmemset(dst, val, len); }
