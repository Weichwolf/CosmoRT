/* Dynamic page table management */
#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include "boot_info.h"

/* Rebuild page tables from UEFI memory map.
 * Maps: all EfiConventionalMemory, EfiLoaderCode/Data, EfiBootServicesCode/Data,
 *       EfiRuntimeServicesCode/Data, framebuffer, known MMIO regions.
 * Uses 2MB pages for efficiency. */
void paging_init(struct boot_info *info);

/* Map a specific 2MB-aligned physical address (for MMIO discovery) */
void paging_map_2mb(uint64_t phys_addr);

/* Reserved pages (carved by paging_init for PD tables).
 * Buddy allocator must NOT use these. */
#define PAGING_MAX_RESERVED 512
extern uint64_t paging_reserved_phys[PAGING_MAX_RESERVED];
extern int paging_reserved_count;

#endif
