/* CosmoRT Process — demand paging, lazyfree, free_address_space, VMA helpers */

#include "proc/proc_internal.h"

/* ── PTE read helper ─────────────────────────────── */

uint64_t read_pte(uint64_t *user_pml4, uint64_t va) {
    return read_pte_pub(user_pml4, va);
}

/* ── LAZYFREE reclaim — called by page_alloc on OOM ── */

/* Reclaim up to 'count' clean LAZYFREE pages across all processes.
 * Returns number of pages actually freed. Caller must NOT hold buddy_lock. */
int lazyfree_reclaim(int count) {
    int freed = 0;
    for (int pid = 1; pid < PID_TABLE_MAX && freed < count; pid++) {
        process_t *p = pid_table[pid];
        if (!p || !p->pml4) continue;
        uint64_t irqf;
        if (!spin_trylock_irq(&p->lock, &irqf)) continue;

        /* Walk all VMAs looking for anonymous writable regions */
        vma_t *stack[64];
        int sp = 0;
        vma_t *cur = p->vma_root;
        while ((cur || sp > 0) && freed < count) {
            while (cur) {
                if (sp < 64) stack[sp++] = cur;
                cur = cur->left;
            }
            if (sp <= 0) break;
            cur = stack[--sp];
            vma_t *v = cur;
            cur = cur->right;
            if (!(v->flags & MAP_ANONYMOUS) || !(v->prot & PROT_WRITE))
                continue;
            /* Scan pages in this VMA */
            for (uint64_t va = v->start; va < v->end && freed < count; va += 4096) {
                int pml4i = (va >> 39) & 0x1FF;
                if (!(p->pml4[pml4i] & PTE_PRESENT)) {
                    va = (va | ((1ULL << 39) - 1));
                    continue;
                }
                uint64_t *pdpt = (uint64_t *)phys_to_virt(p->pml4[pml4i] & PTE_ADDR_MASK);
                int pdpti = (va >> 30) & 0x1FF;
                if (!(pdpt[pdpti] & PTE_PRESENT)) {
                    va = (va | ((1ULL << 30) - 1));
                    continue;
                }
                uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PTE_ADDR_MASK);
                int pdi = (va >> 21) & 0x1FF;
                if (!(pd[pdi] & PTE_PRESENT)) {
                    va = (va | ((1ULL << 21) - 1));
                    continue;
                }
                /* Huge pages are never LAZYFREE (split before marking) — skip */
                if (pd[pdi] & PTE_PS) {
                    va = (va | ((1ULL << 21) - 1));
                    continue;
                }
                uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PTE_ADDR_MASK);
                int pti = (va >> 12) & 0x1FF;
                uint64_t pte = pt[pti];
                /* Reclaimable: present + LAZYFREE + NOT dirty */
                if ((pte & PTE_PRESENT) && (pte & PTE_LAZYFREE) && !(pte & PTE_DIRTY)) {
                    uint64_t phys = pte & PTE_ADDR_MASK;
                    pt[pti] = 0;
                    arch_invlpg(va);
                    page_free(phys_to_virt(phys));
                    freed++;
                }
            }
        }
        spin_unlock_irq(&p->lock, irqf);
    }
    return freed;
}

/* ── Free address space ──────────────────────────── */

/* Free all user pages and page table pages under a PML4.
 * User data pages use page_free (respects refcount for COW).
 * Page table structure pages (PT/PD/PDPT/PML4) are always single-owner. */
void free_address_space(uint64_t *user_pml4) {
    if (!user_pml4) return;

    /* Flush TLB on other cores that may have this PML4 cached */
    tlb_shootdown(virt_to_phys(user_pml4));

    /* Walk lower half only (PML4[0..255] = user space) */
    for (int i = 0; i < 256; i++) {
        if (!(user_pml4[i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[i] & PTE_ADDR_MASK);

        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) continue;
            uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[j] & PTE_ADDR_MASK);

            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT)) continue;

                /* Huge page (2MB PMD with PS bit): free 512 sub-pages */
                if (pd[k] & PTE_PS) {
                    uint64_t huge_phys = pd[k] & 0x000FFFFFFFE00000ULL;
                    for (int l = 0; l < 512; l++)
                        page_free(phys_to_virt(huge_phys + (uint64_t)l * 4096));
                    continue; /* no PT to free */
                }

                uint64_t *pt = (uint64_t *)phys_to_virt(pd[k] & PTE_ADDR_MASK);

                for (int l = 0; l < 512; l++) {
                    if (pt[l] & PTE_PRESENT) {
                        /* page_free handles refcount: shared COW pages survive */
                        page_free(phys_to_virt(pt[l] & PTE_ADDR_MASK));
                    }
                }
                page_free(pt); /* free PT page */
            }
            page_free(pd); /* free PD page */
        }
        page_free(pdpt); /* free PDPT page */
    }
    page_free(user_pml4); /* free PML4 page */
}

/* Free all VMAs in a tree */
void vma_free_tree(vma_t *node) {
    if (!node) return;
    vma_free_tree(node->left);
    vma_free_tree(node->right);
    vma_free(node);
}

/* Clear PTEs for shared VMAs without freeing physical pages (owner frees them).
 * Must run BEFORE free_address_space which frees every present page.
 * Write-back dirty pages for file-backed shared VMAs before clearing PTEs. */
void unmap_shared_vmas(vma_t *node, uint64_t *user_pml4) {
    if (!node || !user_pml4) return;
    unmap_shared_vmas(node->left, user_pml4);
    if (node->flags & VMA_SHARED) {
        for (uint64_t va = node->start; va < node->end; va += 4096) {
            int pml4i = (va >> 39) & 0x1FF;
            if (!(user_pml4[pml4i] & PTE_PRESENT)) continue;
            uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & PTE_ADDR_MASK);
            int pdpti = (va >> 30) & 0x1FF;
            if (!(pdpt[pdpti] & PTE_PRESENT)) continue;
            uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PTE_ADDR_MASK);
            int pdi = (va >> 21) & 0x1FF;
            if (!(pd[pdi] & PTE_PRESENT)) continue;
            uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PTE_ADDR_MASK);
            int pti = (va >> 12) & 0x1FF;
            uint64_t pte = pt[pti];
            /* Write-back dirty file-backed pages before clearing */
            if ((pte & PTE_PRESENT) && (pte & PTE_DIRTY) && node->file_ino) {
                uint64_t phys = pte & PTE_ADDR_MASK;
                uint64_t foff = node->file_offset + (va - node->start);
                extern long vfs_pwrite_by_ino(int, uint64_t, const void *, size_t, size_t);
                vfs_pwrite_by_ino(node->file_backend, node->file_ino,
                                  phys_to_virt(phys), (size_t)foff, 4096);
            }
            pt[pti] = 0;  /* clear PTE, don't free the page */
        }
    }
    unmap_shared_vmas(node->right, user_pml4);
}
