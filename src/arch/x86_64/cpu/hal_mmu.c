/* x86_64 HAL - MMU hardware primitives.
 *
 * High-level page-table manipulation (map/unmap/alloc_pgdir) still lives
 * in src/kernel/mm/paging.c. This file covers the arch-specific atoms
 * the rest of the kernel must not touch directly: CR3, INVLPG, CR2.
 */

#include "hal/hal_mmu.h"
#include "arch/arch.h"

void hal_mmu_switch(paddr_t phys_pgd) {
    arch_set_cr3(phys_pgd);
}

void hal_mmu_flush(vaddr_t va) {
    arch_invlpg(va);
}

void hal_mmu_flush_range(vaddr_t start, size_t len) {
    const size_t page = 4096;
    vaddr_t end = (start + len + (page - 1)) & ~(page - 1);
    for (vaddr_t v = start & ~(page - 1); v < end; v += page)
        arch_invlpg(v);
}

void hal_mmu_flush_all(void) {
    arch_flush_tlb();
}

vaddr_t hal_mmu_fault_address(void) {
    return arch_get_cr2();
}
