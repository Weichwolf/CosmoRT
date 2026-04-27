/* CosmoRT GUP — get_user_pages-equivalent. See include/kernel/mm/gup.h.
 *
 * Why this exists: futex_va_to_pa walks the user PT to derive the SHARED
 * key. FUTEX_WAKE never reads *uaddr, unlike FUTEX_WAIT. A child that only
 * WAKEs on a MAP_SHARED page that parent demand-faulted (and child still
 * has unpopulated) computes a different key and the wake is lost. Linux
 * solves this with get_user_pages inside futex_get_key. We reproduce the
 * same demand-fault as the page-fault handler, just driven from process
 * context with proper p->lock acquisition, and only in the slow path
 * (called when futex_va_to_pa returned 0). */

#include "mm/gup.h"
#include "mm/vma.h"
#include "mm/page_cache.h"
#include "mm/page_alloc.h"
#include "proc/process.h"
#include "proc/proc_internal.h"
#include "fs/vfs.h"
#include "linux/mman.h"
#include "spinlock.h"
#include "config.h"

#define PTE_PRESENT_BIT 0x1
#define PTE_PS_BIT      0x80
#define PTE_PHYS_MASK   0x000FFFFFFFFFF000ULL

/* Walk p->pml4 and resolve va to its PA. Returns 0 if any level is not
 * present. Mirrors futex.c's local walker but stays a private helper here
 * to keep mm/ self-contained. */
static uint64_t pt_va_to_pa(uint64_t *pml4, uint64_t va) {
    if (!pml4) return 0;
    int i4 = (va >> 39) & 0x1FF;
    if (!(pml4[i4] & PTE_PRESENT_BIT)) return 0;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[i4] & PTE_PHYS_MASK);
    int i3 = (va >> 30) & 0x1FF;
    if (!(pdpt[i3] & PTE_PRESENT_BIT)) return 0;
    if (pdpt[i3] & PTE_PS_BIT)
        return (pdpt[i3] & PTE_PHYS_MASK) | (va & 0x3FFFFFFF);
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[i3] & PTE_PHYS_MASK);
    int i2 = (va >> 21) & 0x1FF;
    if (!(pd[i2] & PTE_PRESENT_BIT)) return 0;
    if (pd[i2] & PTE_PS_BIT)
        return (pd[i2] & PTE_PHYS_MASK) | (va & 0x1FFFFF);
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[i2] & PTE_PHYS_MASK);
    int i1 = (va >> 12) & 0x1FF;
    if (!(pt[i1] & PTE_PRESENT_BIT)) return 0;
    return (pt[i1] & PTE_PHYS_MASK) | (va & 0xFFF);
}

uint64_t mm_gup_one(process_t *p, uint64_t va, int write) {
    if (!p || !p->pml4) return 0;
    uint64_t page_va = va & ~0xFFFULL;

    /* Fast path: already mapped */
    uint64_t pa = pt_va_to_pa(p->pml4, va);
    if (pa) {
        /* Read-only request always wins. Write request on a CoW page must
         * not be silently broken here — caller (futex) only reads. */
        (void)write;
        return pa;
    }

    /* Slow path: pull VMA snapshot under p->lock, then drop the lock for
     * page allocation / page_cache I/O. Re-acquire only for the final
     * map_user_page so we don't deadlock with peers. */
    uint64_t flags;
    spin_lock_irq(&p->lock, &flags);
    vma_t *vma = vma_find(p->vma_root, page_va);
    if (!vma) { spin_unlock_irq(&p->lock, flags); return 0; }
    int prot = vma->prot;
    if ((prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0) {
        spin_unlock_irq(&p->lock, flags);
        return 0;
    }
    uint64_t vma_start = vma->start;
    uint64_t file_ino = vma->file_ino;
    uint64_t file_offset = vma->file_offset;
    int file_backend = vma->file_backend;
    int shared = vma->flags & VMA_SHARED;
    spin_unlock_irq(&p->lock, flags);

    int map_prot = prot;
    if (shared && (prot & PROT_WRITE))
        map_prot = prot & ~PROT_WRITE;

    uint64_t phys = 0;
    if (file_ino) {
        if (shared) phys = page_cache_lookup(file_ino, file_offset + (page_va - vma_start));
        if (phys) {
            page_incref(phys);
            spin_lock_irq(&p->lock, &flags);
            int rc = map_user_page(p->pml4, page_va, phys, map_prot);
            spin_unlock_irq(&p->lock, flags);
            if (rc != 0) { page_free(phys_to_virt(phys)); return 0; }
            return pt_va_to_pa(p->pml4, va);
        }
        uint64_t *pg = alloc_page();
        if (!pg) return 0;
        uint64_t foff = file_offset + (page_va - vma_start);
        vfs_pread_by_ino(file_backend, file_ino, pg, (size_t)foff, 4096);
        phys = virt_to_phys(pg);
        if (shared) page_cache_insert(file_ino, foff, phys);
        spin_lock_irq(&p->lock, &flags);
        int rc = map_user_page(p->pml4, page_va, phys, map_prot);
        spin_unlock_irq(&p->lock, flags);
        if (rc != 0) { page_free(pg); return 0; }
        return pt_va_to_pa(p->pml4, va);
    }

    /* Anonymous: alloc a zeroed page and map it. */
    uint64_t *pg = alloc_page();
    if (!pg) return 0;
    phys = virt_to_phys(pg);
    spin_lock_irq(&p->lock, &flags);
    int rc = map_user_page(p->pml4, page_va, phys, map_prot);
    spin_unlock_irq(&p->lock, flags);
    if (rc != 0) { page_free(pg); return 0; }
    return pt_va_to_pa(p->pml4, va);
}
