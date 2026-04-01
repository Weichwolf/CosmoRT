/* HAL — MMU / Page Tables (arch-agnostic)
 *
 * Abstracts PML4 (x86) / translation tables (ARM).
 * Core MM uses hal_mmu for all page table operations.
 */
#ifndef HAL_MMU_H
#define HAL_MMU_H

#include <stdint.h>
#include <stddef.h>

/* Page directory handle (opaque to kernel, PML4* on x86, pgd* on ARM) */
typedef uint64_t *pgdir_t;

/* Virtual / Physical address types */
typedef uint64_t vaddr_t;
typedef uint64_t paddr_t;

/* Map a page: va → pa with protection flags */
int  hal_mmu_map(pgdir_t pgd, vaddr_t va, paddr_t pa, int prot);

/* Unmap a page */
int  hal_mmu_unmap(pgdir_t pgd, vaddr_t va);

/* Map a huge page (2MB on x86, section on ARM) */
int  hal_mmu_map_huge(pgdir_t pgd, vaddr_t va, paddr_t pa, int prot);

/* Flush TLB for a specific address */
void hal_mmu_flush(vaddr_t va);

/* Flush TLB for a range */
void hal_mmu_flush_range(vaddr_t start, size_t len);

/* Full TLB flush (used on CR3 switch) */
void hal_mmu_flush_all(void);

/* TLB shootdown: flush on all CPUs */
void hal_mmu_shootdown(paddr_t pgd_phys);

/* Allocate/free page directory */
pgdir_t hal_mmu_alloc_pgdir(void);
void    hal_mmu_free_pgdir(pgdir_t pgd);

/* Switch active address space */
void hal_mmu_switch(pgdir_t pgd);

/* Read PTE for a virtual address (0 if not mapped) */
uint64_t hal_mmu_read_pte(pgdir_t pgd, vaddr_t va);

#endif
