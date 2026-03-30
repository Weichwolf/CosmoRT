/* CosmoRT Page Table Management — full physical RAM, no limits */

#include "mm/paging.h"
#include "hw/serial.h"
#include "config.h"
#include "arch/arch.h"
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

#define MAX_PDPT_ENTRIES 2048
static uint64_t all_pd[MAX_PDPT_ENTRIES][512] __attribute__((aligned(4096)));
static uint64_t all_pdpt[4][512] __attribute__((aligned(4096)));

uint64_t paging_reserved_phys[PAGING_MAX_RESERVED];
int paging_reserved_count = 0;

static void serial_dec(uint64_t v) {
    char t[20]; int i = 0;
    do { t[i++] = '0' + (char)(v % 10); v /= 10; } while (v);
    while (i--) serial_putchar(t[i]);
}

void paging_map_2mb(uint64_t phys_addr) {
    uint64_t aligned = phys_addr & ~0x1FFFFFULL;
    uint32_t global_pdpt = (uint32_t)(aligned >> 30);
    uint32_t pd_idx = (uint32_t)((aligned >> 21) & 0x1FF);

    if (global_pdpt >= MAX_PDPT_ENTRIES) return;

    uint32_t pml4_idx = global_pdpt / 512;
    uint32_t pdpt_idx = global_pdpt % 512;
    if (pml4_idx >= 4) return;

    if (!(pml4[pml4_idx] & PTE_PRESENT)) {
        uint64_t pdpt_phys = virt_to_phys(&all_pdpt[pml4_idx][0]);
        pml4[pml4_idx] = pdpt_phys | PTE_PRESENT | PTE_WRITE;
        pml4[256 + pml4_idx] = pdpt_phys | PTE_PRESENT | PTE_WRITE;
    }

    if (!(all_pdpt[pml4_idx][pdpt_idx] & PTE_PRESENT)) {
        uint64_t pd_phys = virt_to_phys(&all_pd[global_pdpt][0]);
        all_pdpt[pml4_idx][pdpt_idx] = pd_phys | PTE_PRESENT | PTE_WRITE;
    }

    all_pd[global_pdpt][pd_idx] = aligned | PAGE_MMIO;

    arch_invlpg(aligned);
    arch_invlpg(aligned + PHYS_OFFSET);
}

void paging_init(struct boot_info *info) {
    uint8_t *mmap = (uint8_t *)phys_to_virt(info->mmap_addr);
    uint64_t desc_size = info->mmap_desc_size;
    uint64_t count = info->mmap_size / desc_size;

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
    if (pdpt_needed < 8) pdpt_needed = 8;
    if (pdpt_needed > MAX_PDPT_ENTRIES) pdpt_needed = MAX_PDPT_ENTRIES;
    uint32_t pml4_entries = (pdpt_needed + 511) / 512;
    if (pml4_entries > 4) pml4_entries = 4;

    extern uint64_t pd[];
    for (uint32_t p = 0; p < 8 && p < pdpt_needed; p++) {
        for (int j = 0; j < 512; j++)
            all_pd[p][j] = pd[p * 512 + j];
    }

    for (uint32_t p = 8; p < pdpt_needed; p++) {
        for (int j = 0; j < 512; j++) {
            uint64_t addr = ((uint64_t)p << 30) | ((uint64_t)j << 21);
            all_pd[p][j] = addr | PAGE_RAM;
        }
    }

    for (uint32_t m = 0; m < pml4_entries; m++) {
        for (int p = 0; p < 512; p++) {
            uint32_t global_pdpt = m * 512 + (uint32_t)p;
            if (global_pdpt < pdpt_needed) {
                all_pdpt[m][p] = virt_to_phys(&all_pd[global_pdpt][0])
                                 | PTE_PRESENT | PTE_WRITE;
            } else {
                all_pdpt[m][p] = 0;
            }
        }
        uint64_t pdpt_phys = virt_to_phys(&all_pdpt[m][0]);
        pml4[m] = pdpt_phys | PTE_PRESENT | PTE_WRITE;
        pml4[256 + m] = pdpt_phys | PTE_PRESENT | PTE_WRITE;
    }

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

    all_pd[0][0] = 0 | PAGE_RAM;

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

    { uint32_t pi = 0xFEC00000 >> 30, di = (0xFEC00000 >> 21) & 0x1FF;
      all_pd[pi][di] = 0xFEC00000ULL | PAGE_MMIO; }
    { uint32_t pi = 0xFEE00000 >> 30, di = (0xFEE00000 >> 21) & 0x1FF;
      all_pd[pi][di] = 0xFEE00000ULL | PAGE_MMIO; }

    arch_set_cr3(virt_to_phys(pml4));

    serial_puts("Paging: ");
    serial_dec(highest_phys / (1024 * 1024));
    serial_puts(" MB phys, ");
    serial_dec(pdpt_needed);
    serial_puts(" GB direct map, ");
    serial_dec(total_mapped / (1024 * 1024));
    serial_puts(" MB mapped\n");
}
