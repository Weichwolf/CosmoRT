/* Dynamic page table management — builds from UEFI memory map
 *
 * Uses 2MB pages (PS bit in PDE). Identity-mapped.
 * Page table hierarchy: PML4 → PDPT → PD → 2MB pages
 *
 * Page tables are allocated from a static area (defined in entry.asm)
 * and rebuilt based on what actually exists in the system.
 */

#include "paging.h"
#include "serial.h"
#include "config.h"
#include <stdint.h>

/* Page table entries from entry.asm (referenced externally) */
extern uint64_t pml4[];
extern uint64_t pdpt[];
extern uint64_t pd[];

/* Page flags */
#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITE    (1ULL << 1)
#define PTE_PS       (1ULL << 7)  /* 2MB page */
#define PTE_PCD      (1ULL << 4)  /* Page Cache Disable (for MMIO) */
#define PTE_PWT      (1ULL << 3)  /* Page Write-Through */

/* 2MB page flags for RAM */
#define PAGE_RAM  (PTE_PRESENT | PTE_WRITE | PTE_PS)
/* 2MB page flags for MMIO (uncacheable) */
#define PAGE_MMIO (PTE_PRESENT | PTE_WRITE | PTE_PS | PTE_PCD | PTE_PWT)

/* Max supported: 64 PDPT entries × 512 PD entries = 32768 × 2MB = 64GB */
#define MAX_PD_ENTRIES 32768

static void map_page(uint64_t phys, uint64_t flags) {
    uint32_t pd_idx = (uint32_t)(phys >> 21); /* 2MB page index */
    if (pd_idx >= MAX_PD_ENTRIES) return;

    /* Ensure PDPT entry exists */
    uint32_t pdpt_idx = pd_idx / 512;
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
        uint64_t pd_page = virt_to_phys(&pd[pdpt_idx * 512]);
        pdpt[pdpt_idx] = pd_page | PTE_PRESENT | PTE_WRITE;
    }

    pd[pd_idx] = phys | flags;
}

void paging_map_2mb(uint64_t phys_addr) {
    uint64_t aligned = phys_addr & ~0x1FFFFFULL; /* align to 2MB */
    map_page(aligned, PAGE_MMIO);
    /* Flush TLB for both identity-map and direct-map virtual addresses */
    __asm__ volatile("invlpg (%0)" : : "r"(aligned) : "memory");
    uint64_t high = aligned + PHYS_OFFSET;
    __asm__ volatile("invlpg (%0)" : : "r"(high) : "memory");
}

void paging_init(struct boot_info *info) {
    /* Don't clear existing mappings — they're needed for our running code.
     * Only ADD mappings for regions found in the memory map.
     * This ensures we don't unmap ourselves. */

    /* Parse UEFI memory map and map all usable regions */
    uint8_t *mmap = (uint8_t *)phys_to_virt(info->mmap_addr);
    uint64_t desc_size = info->mmap_desc_size;
    uint64_t count = info->mmap_size / desc_size;
    uint64_t total_mapped = 0;

    for (uint64_t i = 0; i < count; i++) {
        uint32_t type = *(uint32_t *)(mmap + i * desc_size);
        uint64_t phys = *(uint64_t *)(mmap + i * desc_size + 8);
        uint64_t pages = *(uint64_t *)(mmap + i * desc_size + 24);
        uint64_t region_size = pages * 4096;

        /* Map all memory types that we might need to access */
        int should_map = 0;
        switch (type) {
            case 1:  /* EfiLoaderCode */
            case 2:  /* EfiLoaderData */
            case 3:  /* EfiBootServicesCode */
            case 4:  /* EfiBootServicesData */
            case 5:  /* EfiRuntimeServicesCode */
            case 6:  /* EfiRuntimeServicesData */
            case 7:  /* EfiConventionalMemory */
            case 9:  /* EfiACPIReclaimMemory */
            case 10: /* EfiACPINVS */
            case 14: /* EfiPersistentMemory */
                should_map = 1;
                break;
        }
        if (!should_map) continue;

        /* Map in 2MB chunks */
        uint64_t start = phys & ~0x1FFFFFULL;
        uint64_t end = (phys + region_size + 0x1FFFFF) & ~0x1FFFFFULL;
        for (uint64_t addr = start; addr < end; addr += 0x200000) {
            map_page(addr, PAGE_RAM);
            total_mapped += 0x200000;
        }
    }

    /* Always map first 2MB (ISA, legacy I/O, BIOS) */
    map_page(0, PAGE_RAM);

    /* Map framebuffer */
    if (info->fb_addr) {
        uint64_t fb_size = (uint64_t)info->fb_pitch * info->fb_height;
        uint64_t fb_start = info->fb_addr & ~0x1FFFFFULL;
        uint64_t fb_end = (info->fb_addr + fb_size + 0x1FFFFF) & ~0x1FFFFFULL;
        for (uint64_t addr = fb_start; addr < fb_end; addr += 0x200000)
            map_page(addr, PAGE_MMIO);
    }

    /* Map LAPIC (0xFEE00000) and I/O APIC (0xFEC00000) */
    map_page(0xFEC00000, PAGE_MMIO);
    map_page(0xFEE00000, PAGE_MMIO);

    /* Reload CR3 to flush entire TLB (CR3 takes physical address) */
    uint64_t cr3 = virt_to_phys(pml4);
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");

    serial_puts("Paging: ");
    { char t[12]; int ti=0; uint64_t mb=total_mapped/(1024*1024);
      do{t[ti++]='0'+mb%10;mb/=10;}while(mb);
      while(ti--) serial_putchar(t[ti]); }
    serial_puts(" MB mapped from memory map\n");
}
