/* CosmoRT Syscall Layer — memory management syscalls */

#include "internal.h"

/* ── Static page-table helpers (callees first) ──── */

static void unmap_range(uint64_t *user_pml4, uint64_t start, uint64_t end) {
    /* PTE physical address mask: bits 12..51 (strips NX, available bits) */
    const uint64_t PHYS_MASK = 0x000FFFFFFFFFF000ULL;
    for (uint64_t va = start; va < end; va += 4096) {
        int pml4i = (va >> 39) & 0x1FF;
        if (!(user_pml4[pml4i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & PHYS_MASK);

        int pdpti = (va >> 30) & 0x1FF;
        if (!(pdpt[pdpti] & PTE_PRESENT)) continue;
        uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PHYS_MASK);

        int pdi = (va >> 21) & 0x1FF;
        if (!(pd[pdi] & PTE_PRESENT)) continue;
        uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PHYS_MASK);

        int pti = (va >> 12) & 0x1FF;
        if (pt[pti] & PTE_PRESENT) {
            uint64_t phys = pt[pti] & 0x000FFFFFFFFFF000ULL; /* bits 12..51 */
            pt[pti] = 0;
            page_free(phys_to_virt(phys));
            /* TLB flush for this address */
            arch_invlpg(va);
        }
    }
}

static uint64_t prot_to_pte_flags(int prot) {
    /* PROT_NONE → not present (no access at all) */
    if (!(prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) return 0;
    uint64_t flags = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) flags |= PTE_WRITE;
    if (!(prot & PROT_EXEC)) flags |= PTE_NX;
    return flags;
}

static void update_pte_prot(uint64_t *user_pml4, uint64_t start, uint64_t end, int prot) {
    const uint64_t PHYS_MASK = 0x000FFFFFFFFFF000ULL;
    uint64_t new_flags = prot_to_pte_flags(prot);
    for (uint64_t va = start; va < end; va += 4096) {
        int pml4i = (va >> 39) & 0x1FF;
        if (!(user_pml4[pml4i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & PHYS_MASK);

        int pdpti = (va >> 30) & 0x1FF;
        if (!(pdpt[pdpti] & PTE_PRESENT)) continue;
        uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PHYS_MASK);

        int pdi = (va >> 21) & 0x1FF;
        if (!(pd[pdi] & PTE_PRESENT)) continue;
        uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PHYS_MASK);

        int pti = (va >> 12) & 0x1FF;
        uint64_t phys = pt[pti] & PHYS_MASK;
        if (phys) {
            /* Update PTE flags — works for both present and not-present
             * (PROT_NONE sets flags=0, PROT_READ restores PRESENT bit) */
            pt[pti] = phys | new_flags;
            arch_invlpg(va);
        }
    }
}

/* Copy mapped pages from old range to new range (both in same address space).
 * Only copies pages that have PTEs present; unmapped pages stay zero in dst. */
static void copy_user_pages(uint64_t *user_pml4, uint64_t dst, uint64_t src,
                            uint64_t len, int prot) {
    const uint64_t PHYS_MASK = 0x000FFFFFFFFFF000ULL;
    for (uint64_t off = 0; off < len; off += 4096) {
        uint64_t va = src + off;
        /* Walk page tables to find source physical page */
        int pml4i = (va >> 39) & 0x1FF;
        if (!(user_pml4[pml4i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & PHYS_MASK);
        int pdpti = (va >> 30) & 0x1FF;
        if (!(pdpt[pdpti] & PTE_PRESENT)) continue;
        uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PHYS_MASK);
        int pdi = (va >> 21) & 0x1FF;
        if (!(pd[pdi] & PTE_PRESENT)) continue;
        uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PHYS_MASK);
        int pti = (va >> 12) & 0x1FF;
        if (!(pt[pti] & PTE_PRESENT)) continue;

        uint64_t src_phys = pt[pti] & PHYS_MASK;
        void *src_page = phys_to_virt(src_phys);

        /* Allocate new page, copy content, map at dst */
        uint64_t *new_page = alloc_page();
        if (!new_page) continue;
        kmemcpy(new_page, src_page, 4096);
        map_user_page(user_pml4, dst + off, virt_to_phys(new_page), prot);
    }
}

/* ── Cold-path error helpers (keep strings out of hot brk/mmap) ── */

__attribute__((cold))
static void brk_collision_error(uint64_t addr, uint64_t ov_start, uint64_t ov_end) {
    static int brk_coll_cnt;
    if (brk_coll_cnt++ >= 2) return; /* suppress after first 2 */
    serial_puts("brk: ENOMEM collision 0x");
    serial_hex64(addr);
    serial_puts(" vs VMA [0x");
    serial_hex64(ov_start);
    serial_puts(",0x");
    serial_hex64(ov_end);
    serial_puts(")\n");
}

__attribute__((cold))
static void mmap_enomem_error(uint64_t length, uint64_t hint) {
    serial_puts("mmap: ENOMEM len=0x");
    serial_hex64(length);
    serial_puts(" hint=0x");
    serial_hex64(hint);
    serial_putchar('\n');
}

/* ── Pre-fault helper: allocate + map all pages in range ── */

void prefault_range(uint64_t *user_pml4, uint64_t start, uint64_t end, int prot) {
    for (uint64_t va = start; va < end; va += 4096) {
        /* Check if already mapped */
        int pml4i = (va >> 39) & 0x1FF;
        int mapped = 0;
        if (user_pml4[pml4i] & PTE_PRESENT) {
            uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & PTE_ADDR_MASK);
            int pdpti = (va >> 30) & 0x1FF;
            if (pdpt[pdpti] & PTE_PRESENT) {
                uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PTE_ADDR_MASK);
                int pdi = (va >> 21) & 0x1FF;
                if (pd[pdi] & PTE_PRESENT) {
                    uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PTE_ADDR_MASK);
                    int pti = (va >> 12) & 0x1FF;
                    if (pt[pti] & PTE_PRESENT) mapped = 1;
                }
            }
        }
        if (!mapped) {
            uint64_t *page = alloc_page();
            if (page)
                map_user_page(user_pml4, va, virt_to_phys(page), prot);
        }
    }
}

void vma_walk_prefault(vma_t *node, uint64_t *pml4) {
    if (!node) return;
    vma_walk_prefault(node->left, pml4);
    prefault_range(pml4, node->start, node->end, node->prot);
    node->flags |= VMA_LOCKED;
    vma_walk_prefault(node->right, pml4);
}

/* ── SYS_brk (12) ───────────────────────────────── */

__attribute__((hot))
long do_brk(unsigned long addr) {
    process_t *p = proc_current();
    if (__builtin_expect(!p, 0)) return -EFAULT;
    if (addr == 0) return (long)p->brk_current;
    if (addr < p->brk_base) return (long)p->brk_current;
    if (addr >= 0x800000000000ULL) return (long)p->brk_current;
    /* Cap brk growth to 256MB above base to prevent excessive virtual memory use */
    if (addr > p->brk_base + (256ULL << 20)) return (long)p->brk_current;
    /* Fast reject: if we previously hit a VMA collision, don't re-scan */
    if (p->brk_ceiling && addr >= p->brk_ceiling) return (long)p->brk_current;

    uint64_t flags;
    spin_lock_irq(&p->lock, &flags);

    uint64_t old_end = (p->brk_current + 0xFFF) & ~0xFFFULL;
    uint64_t new_end = (addr + 0xFFF) & ~0xFFFULL;

    /* Check for overlap with existing VMAs when growing */
    if (new_end > old_end) {
        vma_t *overlap = vma_find_overlap(p->vma_root, old_end, new_end);
        if (overlap && !(overlap->start == p->brk_base)) {
            /* brk would collide with an mmap'd region — refuse.
             * Cache the collision point so future calls return instantly. */
            p->brk_ceiling = overlap->start;
            long ret = (long)p->brk_current;
            uint64_t ov_start = overlap->start, ov_end = overlap->end;
            spin_unlock_irq(&p->lock, flags);
            brk_collision_error(addr, ov_start, ov_end);
            return ret;
        }
    } else if (new_end < old_end) {
        /* Shrinking: update VMA FIRST (so page-fault handler on other cores
         * won't re-allocate pages in the freed range), then unmap + TLB flush */
        vma_t *bv = vma_find(p->vma_root, p->brk_base);
        if (bv && bv->start == p->brk_base) bv->end = new_end;
        unmap_range(p->pml4, new_end, old_end);
        tlb_shootdown(virt_to_phys(p->pml4));
        arch_flush_tlb();
    }

    /* Update brk VMA (for grow cases — shrink already handled above) */
    if (new_end >= old_end) {
        vma_t *brk_vma = vma_find(p->vma_root, p->brk_base);
        if (brk_vma && brk_vma->start == p->brk_base) {
            brk_vma->end = new_end;
        } else if (new_end > p->brk_base) {
            vma_insert(&p->vma_root, p->brk_base, new_end,
                       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
        }
    }

    p->brk_current = addr;
    spin_unlock_irq(&p->lock, flags);
    return (long)addr;
}

/* ── SYS_mlockall (151) / SYS_munlockall (152) ──── */

long do_mlockall(int flags) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    if (flags & ~(MCL_CURRENT | MCL_FUTURE)) return -EINVAL;
    uint64_t irqf;
    spin_lock_irq(&p->lock, &irqf);
    p->mlockall_flags = flags;
    if (flags & MCL_CURRENT)
        vma_walk_prefault(p->vma_root, p->pml4);
    spin_unlock_irq(&p->lock, irqf);
    return 0;
}

long do_munlockall(void) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    p->mlockall_flags = 0;
    return 0;
}

/* ── SYS_mlock (149) / SYS_munlock (150) ─────────── */

long do_mlock(unsigned long addr, size_t len) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    addr &= ~0xFFFULL;
    len = (len + 0xFFF) & ~0xFFFULL;
    uint64_t irqf;
    spin_lock_irq(&p->lock, &irqf);
    for (uint64_t va = addr; va < addr + len; ) {
        vma_t *v = vma_find(p->vma_root, va);
        if (!v) { va += 4096; continue; }
        v->flags |= VMA_LOCKED;
        uint64_t end = v->end < addr + len ? v->end : addr + len;
        prefault_range(p->pml4, va, end, v->prot);
        va = v->end;
    }
    spin_unlock_irq(&p->lock, irqf);
    return 0;
}

long do_munlock(unsigned long addr, size_t len) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    addr &= ~0xFFFULL;
    len = (len + 0xFFF) & ~0xFFFULL;
    uint64_t irqf;
    spin_lock_irq(&p->lock, &irqf);
    for (uint64_t va = addr; va < addr + len; ) {
        vma_t *v = vma_find(p->vma_root, va);
        if (!v) { va += 4096; continue; }
        v->flags &= ~VMA_LOCKED;
        va = v->end;
    }
    spin_unlock_irq(&p->lock, irqf);
    return 0;
}

/* ── SYS_mmap (9) ───────────────────────────────── */

__attribute__((hot))
long do_mmap(unsigned long addr, size_t length, int prot,
                    int flags, int fd, long offset) {
    process_t *p = proc_current();
    if (__builtin_expect(!p, 0)) return -EFAULT;

    /* MAP_SHARED|MAP_ANONYMOUS: mark VMA as shared so fork maps the same
     * physical pages instead of copying (changes visible across processes).
     * File-backed MAP_SHARED not supported — silently downgrade to PRIVATE. */
    if (flags & MAP_SHARED) {
        if (flags & MAP_ANONYMOUS)
            flags |= VMA_SHARED;
        else
            flags = (flags & ~MAP_SHARED) | MAP_PRIVATE;
    }

    /* Validate file-backed mmap parameters */
    int is_file = (fd >= 0 && !(flags & MAP_ANONYMOUS));
    struct vfs_file *vf = 0;
    if (is_file) {
        fd_entry_t *fde = fd_get(&p->fds, fd);
        if (!fde || fde->type != FD_FILE) return -EBADF;
        vf = (struct vfs_file *)fde->obj;
        if (!vf) return -EBADF;
        if (offset < 0) return -EINVAL;
    }

    length = (length + 0xFFF) & ~0xFFFULL;
    if (length == 0) return -EINVAL;
    if (addr + length < addr) return -EINVAL; /* overflow */

    uint64_t irqf;
    spin_lock_irq(&p->lock, &irqf);

    uint64_t vaddr = 0;
    if (addr && (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE))) {
        vaddr = addr & ~0xFFFULL;
        if (vaddr + length > 0x800000000000ULL) {
            spin_unlock_irq(&p->lock, irqf);
            return -ENOMEM;
        }
        /* MAP_FIXED_NOREPLACE: fail if any overlap exists */
        if (flags & MAP_FIXED_NOREPLACE) {
            if (vma_find_overlap(p->vma_root, vaddr, vaddr + length)) {
                spin_unlock_irq(&p->lock, irqf);
                return -EEXIST;
            }
        }
        /* Remove any overlapping VMAs in [vaddr, vaddr+length) */
        for (;;) {
            vma_t *ov = vma_find_overlap(p->vma_root, vaddr, vaddr + length);
            if (!ov) break;
            if (ov->start < vaddr && ov->end > vaddr + length) {
                uint64_t orig_end = ov->end;
                int saved_prot = ov->prot;
                int saved_flags = ov->flags;
                ov->end = vaddr;
                vma_insert(&p->vma_root, vaddr + length, orig_end,
                           saved_prot, saved_flags);
            } else if (ov->start < vaddr) {
                ov->end = vaddr;
            } else if (ov->end > vaddr + length) {
                ov->start = vaddr + length;
            } else {
                vma_remove(&p->vma_root, ov);
            }
            ov = vma_find(p->vma_root, vaddr);
            if (!ov) break;
            if (ov->start >= vaddr + length) break;
        }
    } else {
        /* Try hint address first if provided.
         * If hint overlaps an existing VMA, search upward from hint
         * for a free region (Linux behavior: hint is a preference,
         * not a hard requirement). */
        if (addr) {
            uint64_t hint = addr & ~0xFFFULL;
            if (!vma_find_overlap(p->vma_root, hint, hint + length)) {
                vaddr = hint;
            } else {
                extern uint64_t vma_find_free_above(vma_t *root, uint64_t start, uint64_t size);
                vaddr = vma_find_free_above(p->vma_root, hint, length);
            }
        }
        if (!vaddr) {
            vaddr = vma_find_free(p->vma_root, p->mmap_next, length);
            if (!vaddr) {
                /* Retry from top — munmap may have freed space above mmap_next */
                vaddr = vma_find_free(p->vma_root, USER_MMAP_BASE, length);
                if (!vaddr) {
                    spin_unlock_irq(&p->lock, irqf);
                    mmap_enomem_error(length, addr);
                    return -ENOMEM;
                }
            }
            p->mmap_next = vaddr;
        }
    }

    /* Create VMA */
    int vma_flags = flags;
    if (p->mlockall_flags & MCL_FUTURE) vma_flags |= VMA_LOCKED;
    vma_t *v = vma_insert(&p->vma_root, vaddr, vaddr + length, prot, vma_flags);
    if (!v) {
        spin_unlock_irq(&p->lock, irqf);
        return -ENOMEM;
    }

    /* File-backed mmap: allocate pages and read file content.
     * Release lock during I/O (vfs_pread may block), then re-acquire
     * and verify the VMA still exists before returning. */
    if (is_file) {
        uint64_t saved_vaddr = vaddr;
        uint64_t saved_length = length;
        spin_unlock_irq(&p->lock, irqf);
        extern long vfs_pread(struct vfs_file *f, void *buf, size_t count, uint64_t off);
        uint64_t file_off = (uint64_t)offset;
        for (uint64_t va = saved_vaddr; va < saved_vaddr + saved_length; va += 4096) {
            uint64_t *pg = alloc_page(); /* zeroed */
            if (!pg) return -ENOMEM;
            /* Read up to 4096 bytes from file at current offset */
            long nread = vfs_pread(vf, pg, 4096, file_off);
            (void)nread; /* short read is fine — rest is zero */
            if (map_user_page(p->pml4, va, virt_to_phys(pg), prot) < 0) {
                page_free(pg);
                return -ENOMEM;
            }
            if (file_off > UINT64_MAX - 4096) break; /* overflow guard */
            file_off += 4096;
        }
        /* Re-acquire lock and verify VMA wasn't torn down by concurrent munmap */
        spin_lock_irq(&p->lock, &irqf);
        vma_t *check = vma_find(p->vma_root, saved_vaddr);
        if (!check || check->start > saved_vaddr ||
            check->end < saved_vaddr + saved_length) {
            /* VMA was torn down — unmap pages we just mapped to prevent leak */
            unmap_range(p->pml4, saved_vaddr, saved_vaddr + saved_length);
            spin_unlock_irq(&p->lock, irqf);
            return -ENOMEM;
        }
        spin_unlock_irq(&p->lock, irqf);
        return (long)saved_vaddr;
    }

    /* Anonymous: pre-fault if locked or MAP_POPULATE */
    if ((vma_flags & VMA_LOCKED) || (flags & MAP_POPULATE))
        prefault_range(p->pml4, vaddr, vaddr + length, prot);

    spin_unlock_irq(&p->lock, irqf);
    return (long)vaddr;
}

/* ── SYS_munmap (11) / SYS_mprotect (10) ────────── */

__attribute__((hot))
long do_munmap(unsigned long addr, size_t length) {
    process_t *p = proc_current();
    if (__builtin_expect(!p, 0)) return -EFAULT;
    if (addr & 0xFFF) return -EINVAL;
    if (addr >= 0x800000000000ULL) return -EINVAL;

    length = (length + 0xFFF) & ~0xFFFULL;
    if (addr + length < addr) return -EINVAL; /* overflow */
    if (addr + length > 0x800000000000ULL) return -EINVAL;
    uint64_t start = addr;
    uint64_t end = addr + length;

    uint64_t irqf;
    spin_lock_irq(&p->lock, &irqf);

    /* Unmap physical pages */
    unmap_range(p->pml4, start, end);
    /* TLB flush: local + remote cores sharing this address space */
    arch_flush_tlb();
    tlb_shootdown(virt_to_phys(p->pml4));

    /* Adjust VMAs: find and remove/split overlapping VMAs */
    for (;;) {
        vma_t *v = vma_find_overlap(p->vma_root, start, end);
        if (!v) break;

        if (v->start >= start && v->end <= end) {
            /* Entirely within unmap range */
            vma_remove(&p->vma_root, v);
        } else if (v->start < start && v->end > end) {
            /* VMA straddles both sides — split */
            uint64_t orig_end = v->end;
            v->end = start;
            vma_insert(&p->vma_root, end, orig_end, v->prot, v->flags);
            break;
        } else if (v->start < start) {
            v->end = start;
        } else {
            v->start = end;
        }
    }

    spin_unlock_irq(&p->lock, irqf);
    return 0;
}

long do_mprotect(unsigned long addr, size_t len, int prot) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    if (addr & 0xFFF) return -EINVAL;
    if (addr >= 0x800000000000ULL) return -ENOMEM;

    len = (len + 0xFFF) & ~0xFFFULL;
    uint64_t start = addr;
    uint64_t end = addr + len;

    uint64_t irqf;
    spin_lock_irq(&p->lock, &irqf);

    /* Update PTE permissions for already-mapped pages */
    update_pte_prot(p->pml4, start, end, prot);

    /* TLB flush: local + remote cores sharing this address space */
    arch_flush_tlb();
    tlb_shootdown(virt_to_phys(p->pml4));

    /* Update VMA prot flags, splitting if needed */
    for (uint64_t probe = start; probe < end; ) {
        vma_t *v = vma_find(p->vma_root, probe);
        if (!v) { probe += 4096; continue; }

        if (v->start < start) {
            /* Split: left part keeps old prot */
            uint64_t orig_end = v->end;
            int orig_prot = v->prot;
            int orig_flags = v->flags;
            v->end = start;
            vma_insert(&p->vma_root, start, orig_end, orig_prot, orig_flags);
            continue; /* re-probe */
        }
        if (v->end > end) {
            /* Split: right part keeps old prot */
            int orig_prot = v->prot;
            int orig_flags = v->flags;
            vma_insert(&p->vma_root, end, v->end, orig_prot, orig_flags);
            v->end = end;
        }
        v->prot = prot;
        probe = v->end;
    }

    spin_unlock_irq(&p->lock, irqf);
    return 0;
}

/* ── SYS_madvise (28) ───────────────────────────── */

#define MADV_DONTNEED 4

long do_madvise(unsigned long addr, size_t length, int advice) {
    if (addr & 0xFFF) return -EINVAL;
    length = (length + 0xFFF) & ~0xFFFULL;

    if (advice == MADV_DONTNEED) {
        /* Drop anonymous pages — next access gets fresh zeros.
         * Only unmap pages in MAP_ANONYMOUS VMAs. File-backed or
         * ELF-loaded pages must not be zeroed (data loss). */
        process_t *p = proc_current();
        if (!p) return -EFAULT;
        uint64_t start = addr;
        uint64_t end = addr + length;
        uint64_t irqf;
        spin_lock_irq(&p->lock, &irqf);
        for (uint64_t va = start; va < end; ) {
            vma_t *v = vma_find(p->vma_root, va);
            if (!v) { va += 4096; continue; }
            if (v->flags & MAP_ANONYMOUS) {
                uint64_t ustart = va > v->start ? va : v->start;
                uint64_t uend = end < v->end ? end : v->end;
                unmap_range(p->pml4, ustart, uend);
            }
            va = v->end;
        }
        arch_flush_tlb();
        extern void tlb_shootdown(uint64_t pml4_phys);
        tlb_shootdown(virt_to_phys(p->pml4));
        spin_unlock_irq(&p->lock, irqf);
    }
    /* All other advice: accept but ignore */
    return 0;
}

/* ── SYS_mremap (25) ────────────────────────────── */

long do_mremap(unsigned long old_addr, size_t old_size, size_t new_size,
               int flags, unsigned long new_addr) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    if (old_addr & 0xFFF) return -EINVAL;

    old_size = (old_size + 0xFFF) & ~0xFFFULL;
    new_size = (new_size + 0xFFF) & ~0xFFFULL;
    if (new_size == 0) return -EINVAL;

    uint64_t irqf;
    spin_lock_irq(&p->lock, &irqf);

    /* Find VMA covering old_addr */
    vma_t *v = vma_find(p->vma_root, old_addr);
    if (!v) { spin_unlock_irq(&p->lock, irqf); return -EFAULT; }
    /* old_addr must be VMA start and old_size must match */
    if (v->start != old_addr) { spin_unlock_irq(&p->lock, irqf); return -EFAULT; }

    /* MREMAP_FIXED not supported yet */
    if (flags & MREMAP_FIXED) {
        (void)new_addr;
        spin_unlock_irq(&p->lock, irqf);
        return -ENOSYS;
    }

    if (new_size == old_size) { spin_unlock_irq(&p->lock, irqf); return (long)old_addr; }

    if (new_size < old_size) {
        /* Shrink: unmap tail pages, adjust VMA */
        uint64_t trim_start = old_addr + new_size;
        uint64_t trim_end = old_addr + old_size;
        unmap_range(p->pml4, trim_start, trim_end);
        tlb_shootdown(virt_to_phys(p->pml4));
        arch_flush_tlb();
        v->end = old_addr + new_size;
        spin_unlock_irq(&p->lock, irqf);
        return (long)old_addr;
    }

    /* Grow: try to expand in-place */
    uint64_t grow_start = old_addr + old_size;
    uint64_t grow_end = old_addr + new_size;

    if (!vma_find_overlap(p->vma_root, grow_start, grow_end)) {
        /* No overlap — expand VMA in place */
        v->end = grow_end;
        spin_unlock_irq(&p->lock, irqf);
        return (long)old_addr;
    }

    /* Can't expand in-place — need MREMAP_MAYMOVE */
    if (!(flags & MREMAP_MAYMOVE)) { spin_unlock_irq(&p->lock, irqf); return -ENOMEM; }

    /* Allocate new region */
    uint64_t new_va = vma_find_free(p->vma_root, p->mmap_next, new_size);
    if (!new_va) {
        new_va = vma_find_free(p->vma_root, USER_MMAP_BASE, new_size);
        if (!new_va) { spin_unlock_irq(&p->lock, irqf); return -ENOMEM; }
    }
    p->mmap_next = new_va;

    int v_prot = v->prot;
    int v_flags = v->flags;

    /* Create new VMA */
    vma_t *nv = vma_insert(&p->vma_root, new_va, new_va + new_size, v_prot, v_flags);
    if (!nv) { spin_unlock_irq(&p->lock, irqf); return -ENOMEM; }

    /* Copy existing pages to new location */
    copy_user_pages(p->pml4, new_va, old_addr, old_size, v_prot);

    /* Unmap old region */
    unmap_range(p->pml4, old_addr, old_addr + old_size);
    arch_flush_tlb();
    tlb_shootdown(virt_to_phys(p->pml4));

    /* Remove old VMA (re-find since tree may have changed) */
    vma_t *old_v = vma_find(p->vma_root, old_addr);
    if (old_v && old_v->start == old_addr)
        vma_remove(&p->vma_root, old_v);

    spin_unlock_irq(&p->lock, irqf);
    return (long)new_va;
}
