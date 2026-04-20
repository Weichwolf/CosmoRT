/* aarch64 HAL - MMU primitives (stubs).
 *
 * Phase-9 placeholder. Real impl will back these with TTBR0_EL1/TTBR1_EL1
 * writes, TLBI instructions, and FAR_EL1 reads.
 *
 * Not linked into the x86_64 build.
 */

#include "hal/hal_mmu.h"
#include "kernel/panic.h"

#define AARCH64_STUB(name) \
    do { (void)0; panic("aarch64 hal_mmu_" #name " not implemented"); } while (0)

int hal_mmu_map(pgdir_t pgd, vaddr_t va, paddr_t pa, int prot) {
    (void)pgd; (void)va; (void)pa; (void)prot;
    AARCH64_STUB(map); return -1;
}
int hal_mmu_unmap(pgdir_t pgd, vaddr_t va)             { (void)pgd; (void)va; AARCH64_STUB(unmap); return -1; }
int hal_mmu_map_huge(pgdir_t p, vaddr_t va, paddr_t pa, int prot) {
    (void)p; (void)va; (void)pa; (void)prot;
    AARCH64_STUB(map_huge); return -1;
}
void hal_mmu_flush(vaddr_t va)                         { (void)va; AARCH64_STUB(flush); }
void hal_mmu_flush_range(vaddr_t s, size_t l)          { (void)s; (void)l; AARCH64_STUB(flush_range); }
void hal_mmu_flush_all(void)                           { AARCH64_STUB(flush_all); }
void hal_mmu_shootdown(paddr_t p)                      { (void)p; AARCH64_STUB(shootdown); }
pgdir_t hal_mmu_alloc_pgdir(void)                      { AARCH64_STUB(alloc_pgdir); return 0; }
void hal_mmu_free_pgdir(pgdir_t p)                     { (void)p; AARCH64_STUB(free_pgdir); }
void hal_mmu_switch(paddr_t p)                         { (void)p; AARCH64_STUB(switch); }
vaddr_t hal_mmu_fault_address(void)                    { AARCH64_STUB(fault_address); return 0; }
uint64_t hal_mmu_read_pte(pgdir_t p, vaddr_t va)       { (void)p; (void)va; AARCH64_STUB(read_pte); return 0; }
