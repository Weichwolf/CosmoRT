/* CosmoRT Page Table Management — full physical RAM, no limits
 *
 * entry.asm maps initial 8GB (enough to boot).
 * paging_init extends the direct map to cover ALL physical RAM.
 * PD pages are static (2MB BSS for 512GB — no dynamic allocation needed).
 */

#include "paging.h"
#include "serial.h"
#include "config.h"
#include <stdint.h>

extern uint64_t pml4[];
extern uint64_t pdpt[];

#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITE    (1ULL << 1)
#define PTE_PS       (1ULL << 7)
#define PTE_PCD      (1ULL << 4)
#define PTE_PWT      (1ULL << 3)

#define PAGE_RAM  (PTE_PRESENT | PTE_WRITE | PTE_PS)
#define PAGE_MMIO (PTE_PRESENT | PTE_WRITE | PTE_PS | PTE_PCD | PTE_PWT)

/* All PD pages in BSS: 512 × 4KB = 2MB. Covers 512 × 1GB = 512GB.
 * No dynamic allocation, no carving, no conflicts with buddy. */
static uint64_t all_pd[512][512] __attribute__((aligned(4096)));

/* Exported reserved list (empty — no carving needed) */
uint64_t paging_reserved_phys[PAGING_MAX_RESERVED];
int paging_reserved_count = 0;

static void serial_dec(uint64_t v) {
    char t[20]; int i = 0;
    do { t[i++] = '0' + (char)(v % 10); v /= 10; } while (v);
    while (i--) serial_putchar(t[i]);
}

void paging_map_2mb(uint64_t phys_addr) {
    uint64_t aligned = phys_addr & ~0x1FFFFFULL;
    uint32_t pdpt_idx = (uint32_t)(aligned >> 30);
    uint32_t pd_idx = (uint32_t)((aligned >> 21) & 0x1FF);

    if (pdpt_idx >= 512) return;

    /* Ensure PDPT entry points to our static PD page */
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
        uint64_t pd_phys = virt_to_phys(&all_pd[pdpt_idx][0]);
        pdpt[pdpt_idx] = pd_phys | PTE_PRESENT | PTE_WRITE;
    }

    all_pd[pdpt_idx][pd_idx] = aligned | PAGE_MMIO;

    __asm__ volatile("invlpg (%0)" : : "r"(aligned) : "memory");
    __asm__ volatile("invlpg (%0)" : : "r"(aligned + PHYS_OFFSET) : "memory");
}

void paging_init(struct boot_info *info) {
    uint8_t *mmap = (uint8_t *)phys_to_virt(info->mmap_addr);
    uint64_t desc_size = info->mmap_desc_size;
    uint64_t count = info->mmap_size / desc_size;

    /* Find highest physical address */
    uint64_t highest_phys = 0;
    for (uint64_t i = 0; i < count; i++) {
        uint32_t type = *(uint32_t *)(mmap + i * desc_size);
        uint64_t phys = *(uint64_t *)(mmap + i * desc_size + 8);
        uint64_t pages = *(uint64_t *)(mmap + i * desc_size + 24);
        if (type >= 1 && type <= 14) {
            uint64_t end = phys + pages * 4096;
            if (end > highest_phys) highest_phys = end;
        }
    }

    uint32_t pdpt_needed = (uint32_t)((highest_phys + (1ULL << 30) - 1) >> 30);
    if (pdpt_needed > 512) pdpt_needed = 512;

    /* Copy initial 8 PD pages from entry.asm into our static array,
     * then wire ALL PDPT entries to static all_pd pages. */
    extern uint64_t pd[]; /* entry.asm initial 8 PD pages */
    for (uint32_t p = 0; p < 8 && p < pdpt_needed; p++) {
        for (int j = 0; j < 512; j++)
            all_pd[p][j] = pd[p * 512 + j];
    }

    /* Fill remaining PD pages with 2MB identity-mapped entries */
    for (uint32_t p = 8; p < pdpt_needed; p++) {
        for (int j = 0; j < 512; j++) {
            uint64_t addr = ((uint64_t)p << 30) | ((uint64_t)j << 21);
            all_pd[p][j] = addr | PAGE_RAM;
        }
    }

    /* Wire PDPT entries to our static PD pages */
    for (uint32_t p = 0; p < pdpt_needed; p++) {
        uint64_t pd_phys = virt_to_phys(&all_pd[p][0]);
        pdpt[p] = pd_phys | PTE_PRESENT | PTE_WRITE;
    }

    /* Map all UEFI regions */
    uint64_t total_mapped = 0;
    for (uint64_t i = 0; i < count; i++) {
        uint32_t type = *(uint32_t *)(mmap + i * desc_size);
        uint64_t phys = *(uint64_t *)(mmap + i * desc_size + 8);
        uint64_t pages = *(uint64_t *)(mmap + i * desc_size + 24);
        uint64_t region_size = pages * 4096;

        int should_map = 0;
        switch (type) {
            case 1: case 2: case 3: case 4: case 5: case 6:
            case 7: case 9: case 10: case 14:
                should_map = 1; break;
        }
        if (!should_map) continue;

        uint64_t start = phys & ~0x1FFFFFULL;
        uint64_t end = (phys + region_size + 0x1FFFFF) & ~0x1FFFFFULL;
        for (uint64_t addr = start; addr < end; addr += 0x200000) {
            uint32_t pi = (uint32_t)(addr >> 30);
            uint32_t di = (uint32_t)((addr >> 21) & 0x1FF);
            if (pi < pdpt_needed)
                all_pd[pi][di] = addr | PAGE_RAM;
            total_mapped += 0x200000;
        }
    }

    /* First 2MB always mapped */
    all_pd[0][0] = 0 | PAGE_RAM;

    /* Framebuffer */
    if (info->fb_addr) {
        uint64_t fb_size = (uint64_t)info->fb_pitch * info->fb_height;
        uint64_t fb_start = info->fb_addr & ~0x1FFFFFULL;
        uint64_t fb_end = (info->fb_addr + fb_size + 0x1FFFFF) & ~0x1FFFFFULL;
        for (uint64_t addr = fb_start; addr < fb_end; addr += 0x200000) {
            uint32_t pi = (uint32_t)(addr >> 30);
            uint32_t di = (uint32_t)((addr >> 21) & 0x1FF);
            if (pi < 512) all_pd[pi][di] = addr | PAGE_MMIO;
        }
    }

    /* LAPIC + IOAPIC */
    { uint32_t pi = 0xFEC00000 >> 30, di = (0xFEC00000 >> 21) & 0x1FF;
      all_pd[pi][di] = 0xFEC00000ULL | PAGE_MMIO; }
    { uint32_t pi = 0xFEE00000 >> 30, di = (0xFEE00000 >> 21) & 0x1FF;
      all_pd[pi][di] = 0xFEE00000ULL | PAGE_MMIO; }

    /* Flush TLB */
    __asm__ volatile("mov %0, %%cr3" : : "r"(virt_to_phys(pml4)) : "memory");

    serial_puts("Paging: ");
    serial_dec(highest_phys / (1024 * 1024));
    serial_puts(" MB phys, ");
    serial_dec(pdpt_needed);
    serial_puts(" GB direct map, ");
    serial_dec(total_mapped / (1024 * 1024));
    serial_puts(" MB mapped\n");
}
