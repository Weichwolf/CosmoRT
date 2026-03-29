/* CosmoRT Syscall Layer — memory management syscalls */

#include "internal.h"
#include "mm/page_cache.h"

/* ── Static page-table helpers (callees first) ──── */

/* Split a 2MB huge page PMD entry into 512 × 4KB PTEs.
 * pd[pdi] must be a present entry with PTE_PS set.
 * After split: pd[pdi] points to a new PT, PS bit cleared.
 * prot_flags: PTE flags to apply (PTE_PRESENT|PTE_USER|...).
 *             If 0, inherits from the PMD entry flags. */
static int split_huge_pmd(uint64_t *pd, int pdi, uint64_t prot_override) {
    uint64_t pmd = pd[pdi];
    if (!(pmd & PTE_PRESENT) || !(pmd & PTE_PS)) return -1;

    uint64_t huge_phys = pmd & HUGE_PAGE_MASK;
    /* Inherit permission flags from PMD if no override */
    uint64_t flags = prot_override;
    if (!flags)
        flags = pmd & (PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX | PTE_COW);

    /* Allocate PT page */
    uint64_t *pt = alloc_page();
    if (!pt) return -1;

    /* Fill 512 PTEs pointing to individual 4KB sub-pages */
    for (int i = 0; i < 512; i++) {
        uint64_t sub_phys = huge_phys + (uint64_t)i * 4096;
        pt[i] = sub_phys | flags;
        /* Each sub-page inherits the refcount of the huge page.
         * The huge page already has refcount set on all 512 sub-pages. */
    }

    /* Replace PMD entry: now points to PT, no PS bit */
    pd[pdi] = virt_to_phys(pt) | PTE_PRESENT | PTE_WRITE | PTE_USER;
    return 0;
}

static void unmap_range(uint64_t *user_pml4, uint64_t start, uint64_t end) {
    const uint64_t PHYS_MASK = 0x000FFFFFFFFFF000ULL;
    for (uint64_t va = start; va < end; ) {
        int pml4i = (va >> 39) & 0x1FF;
        if (!(user_pml4[pml4i] & PTE_PRESENT)) {
            va = (va | ((1ULL << 39) - 1)) + 1;
            continue;
        }
        uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & PHYS_MASK);

        int pdpti = (va >> 30) & 0x1FF;
        if (!(pdpt[pdpti] & PTE_PRESENT)) {
            va = (va | ((1ULL << 30) - 1)) + 1;
            continue;
        }
        uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PHYS_MASK);

        int pdi = (va >> 21) & 0x1FF;
        if (!(pd[pdi] & PTE_PRESENT)) {
            va = (va | ((1ULL << 21) - 1)) + 1;
            continue;
        }

        /* Huge page (2MB PMD entry with PS bit) */
        if (pd[pdi] & PTE_PS) {
            uint64_t huge_base = va & HUGE_PAGE_MASK;
            /* Full 2MB unmap: free entire huge page */
            if (start <= huge_base && end >= huge_base + HUGE_PAGE_SIZE) {
                uint64_t phys = pd[pdi] & HUGE_PAGE_MASK;
                pd[pdi] = 0;
                /* Free all 512 sub-pages via page_free (respects refcount) */
                for (int i = 0; i < 512; i++) {
                    page_free(phys_to_virt(phys + (uint64_t)i * 4096));
                    arch_invlpg(huge_base + (uint64_t)i * 4096);
                }
                va = huge_base + HUGE_PAGE_SIZE;
                continue;
            }
            /* Partial unmap: split first, then unmap individual 4KB pages */
            if (split_huge_pmd(pd, pdi, 0) < 0) {
                va = huge_base + HUGE_PAGE_SIZE;
                continue;
            }
            /* Fall through to 4KB path */
        }

        uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PHYS_MASK);

        int pti = (va >> 12) & 0x1FF;
        if (pt[pti] & PTE_PRESENT) {
            uint64_t phys = pt[pti] & PHYS_MASK;
            pt[pti] = 0;
            page_free(phys_to_virt(phys));
            arch_invlpg(va);
        }
        va += 4096;
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
    for (uint64_t va = start; va < end; ) {
        int pml4i = (va >> 39) & 0x1FF;
        if (!(user_pml4[pml4i] & PTE_PRESENT)) {
            va = (va | ((1ULL << 39) - 1)) + 1;
            continue;
        }
        uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & PHYS_MASK);

        int pdpti = (va >> 30) & 0x1FF;
        if (!(pdpt[pdpti] & PTE_PRESENT)) {
            va = (va | ((1ULL << 30) - 1)) + 1;
            continue;
        }
        uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PHYS_MASK);

        int pdi = (va >> 21) & 0x1FF;
        if (!(pd[pdi] & PTE_PRESENT)) {
            va = (va | ((1ULL << 21) - 1)) + 1;
            continue;
        }

        /* Huge page: if entire 2MB range covered, update PMD in-place.
         * Otherwise split first. */
        if (pd[pdi] & PTE_PS) {
            uint64_t huge_base = va & HUGE_PAGE_MASK;
            if (start <= huge_base && end >= huge_base + HUGE_PAGE_SIZE) {
                /* Full 2MB mprotect: update PMD flags directly */
                uint64_t phys = pd[pdi] & HUGE_PAGE_MASK;
                pd[pdi] = phys | new_flags | PTE_PS;
                for (uint64_t a = huge_base; a < huge_base + HUGE_PAGE_SIZE; a += 4096)
                    arch_invlpg(a);
                va = huge_base + HUGE_PAGE_SIZE;
                continue;
            }
            /* Partial mprotect: split, then update individual PTEs */
            if (split_huge_pmd(pd, pdi, 0) < 0) {
                va = huge_base + HUGE_PAGE_SIZE;
                continue;
            }
            /* Fall through to 4KB path */
        }

        uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PHYS_MASK);

        int pti = (va >> 12) & 0x1FF;
        uint64_t phys = pt[pti] & PHYS_MASK;
        if (phys) {
            pt[pti] = phys | new_flags;
            arch_invlpg(va);
        }
        va += 4096;
    }
}

/* Copy mapped pages from old range to new range (both in same address space).
 * Only copies pages that have PTEs present; unmapped pages stay zero in dst. */
static void copy_user_pages(uint64_t *user_pml4, uint64_t dst, uint64_t src,
                            uint64_t len, int prot) {
    const uint64_t PHYS_MASK = 0x000FFFFFFFFFF000ULL;
    for (uint64_t off = 0; off < len; ) {
        uint64_t va = src + off;
        /* Walk page tables to find source physical page */
        int pml4i = (va >> 39) & 0x1FF;
        if (!(user_pml4[pml4i] & PTE_PRESENT)) {
            uint64_t next = ((va | ((1ULL << 39) - 1)) + 1) - src;
            off = (next > off) ? next : len;
            continue;
        }
        uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & PHYS_MASK);
        int pdpti = (va >> 30) & 0x1FF;
        if (!(pdpt[pdpti] & PTE_PRESENT)) {
            uint64_t next = ((va | ((1ULL << 30) - 1)) + 1) - src;
            off = (next > off) ? next : len;
            continue;
        }
        uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PHYS_MASK);
        int pdi = (va >> 21) & 0x1FF;
        if (!(pd[pdi] & PTE_PRESENT)) {
            uint64_t next = ((va | ((1ULL << 21) - 1)) + 1) - src;
            off = (next > off) ? next : len;
            continue;
        }
        /* Huge page: split before copying individual pages */
        if (pd[pdi] & PTE_PS) {
            split_huge_pmd(pd, pdi, 0);
            if (pd[pdi] & PTE_PS) { /* split failed */
                off += HUGE_PAGE_SIZE;
                continue;
            }
        }
        uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PHYS_MASK);
        int pti = (va >> 12) & 0x1FF;
        if (!(pt[pti] & PTE_PRESENT)) { off += 4096; continue; }

        uint64_t src_phys = pt[pti] & PHYS_MASK;
        void *src_page = phys_to_virt(src_phys);

        /* Allocate new page, copy content, map at dst */
        uint64_t *new_page = alloc_page();
        if (!new_page) { off += 4096; continue; }
        kmemcpy(new_page, src_page, 4096);
        map_user_page(user_pml4, dst + off, virt_to_phys(new_page), prot);
        off += 4096;
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
                    if (pd[pdi] & PTE_PS) { mapped = 1; } /* huge page */
                    else {
                        uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PTE_ADDR_MASK);
                        int pti = (va >> 12) & 0x1FF;
                        if (pt[pti] & PTE_PRESENT) mapped = 1;
                    }
                }
            }
        }
        if (!mapped) {
            /* Try huge page if aligned and enough remaining space */
            uint64_t huge_base = va & HUGE_PAGE_MASK;
            if (va == huge_base && va + HUGE_PAGE_SIZE <= end) {
                void *hp = huge_page_alloc();
                if (hp) {
                    if (map_user_huge_page(user_pml4, va, virt_to_phys(hp), prot) == 0) {
                        va += HUGE_PAGE_SIZE - 4096; /* loop adds 4096 */
                        continue;
                    }
                    huge_page_free(hp);
                }
            }
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
    /* OOM guard: refuse brk growth when memory is critically low */
    if (addr > p->brk_current) {
        extern uint64_t page_free_count(void);
        size_t grow = ((addr - p->brk_current) + 4095) / 4096;
        if (page_free_count() < grow + 256)
            return (long)p->brk_current;
    }
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
            /* mmap(MAP_FIXED) may have replaced the brk VMA with a
             * different prot (e.g. ld-musl creates PROT_NONE gap pages
             * at the end of ELF segments, which can land on brk_base).
             * Remove the foreign VMA and any others in the brk range,
             * then insert a proper brk VMA. */
            if (brk_vma->prot != (PROT_READ | PROT_WRITE) ||
                brk_vma->end < new_end) {
                /* Remove all VMAs in [brk_base, new_end) */
                for (;;) {
                    vma_t *ov = vma_find_overlap(p->vma_root, p->brk_base, new_end);
                    if (!ov) break;
                    if (ov->start >= p->brk_base && ov->end <= new_end) {
                        vma_remove(&p->vma_root, ov);
                    } else if (ov->start < p->brk_base) {
                        ov->end = p->brk_base;
                    } else {
                        /* ov->end > new_end: trim from left */
                        uint64_t ns = new_end, ne = ov->end;
                        int np = ov->prot, nf = ov->flags;
                        vma_remove(&p->vma_root, ov);
                        vma_insert(&p->vma_root, ns, ne, np, nf);
                    }
                }
                vma_insert(&p->vma_root, p->brk_base, new_end,
                           PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
            } else {
                brk_vma->end = new_end;
            }
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
    if (len == 0) return 0;
    /* Overflow / kernel range check */
    if (addr + len < addr || addr + len > 0x800000000000ULL) return -ENOMEM;
    uint64_t irqf;
    uint64_t mlock_end = addr + len;
    spin_lock_irq(&p->lock, &irqf);
    for (uint64_t va = addr; va < mlock_end; ) {
        vma_t *v = vma_find_overlap(p->vma_root, va, mlock_end);
        if (!v) break;
        if (v->start > va) va = v->start;
        v->flags |= VMA_LOCKED;
        uint64_t ve = v->end < mlock_end ? v->end : mlock_end;
        prefault_range(p->pml4, va, ve, v->prot);
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
    if (len == 0) return 0;
    if (addr + len < addr || addr + len > 0x800000000000ULL) return -ENOMEM;
    uint64_t munlock_end = addr + len;
    uint64_t irqf;
    spin_lock_irq(&p->lock, &irqf);
    for (uint64_t va = addr; va < munlock_end; ) {
        vma_t *v = vma_find_overlap(p->vma_root, va, munlock_end);
        if (!v) break;
        if (v->start > va) va = v->start;
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

    /* OOM guard: reject new mappings when physical memory is critically low.
     * Reserves 256 pages (1MB) for kernel operations and process cleanup.
     * Without this, a single process can exhaust all pages, making the
     * entire system unresponsive (no OOM killer implemented yet). */
    {
        extern uint64_t page_free_count(void);
        size_t pages_needed = (length + 4095) / 4096;
        if (!(flags & MAP_FIXED) && page_free_count() < pages_needed + 256)
            return -ENOMEM;
    }

    /* MAP_HUGETLB: no huge page support via this flag — reject */
    if (flags & MAP_HUGETLB) return -EINVAL;

    /* Accept-and-ignore hint flags (no-ops):
     * MAP_NORESERVE (no swap), MAP_GROWSDOWN (stack hint),
     * MAP_DENYWRITE (deprecated), MAP_STACK (hint), MAP_LOCKED (handled below),
     * MAP_32BIT (no x32 compat). Strip to avoid polluting VMA flags. */

    /* MAP_SHARED: mark VMA so fork shares physical pages (not COW).
     * File-backed MAP_SHARED uses page cache for cross-process sharing. */
    if (flags & MAP_SHARED)
        flags |= VMA_SHARED;

    /* /dev/zero mmap → treat as MAP_ANONYMOUS (zero-filled pages) */
    if (fd >= 0 && !(flags & MAP_ANONYMOUS)) {
        fd_entry_t *fde = fd_get(&p->fds, fd);
        if (fde && fde->type == FD_DEVICE &&
            (int)(uintptr_t)fde->obj == 2 /*DEV_ZERO*/) {
            flags |= MAP_ANONYMOUS;
            fd = -1;
        }
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
        /* Unmap old PTEs so demand paging sees the new VMA's file_offset */
        unmap_range(p->pml4, vaddr, vaddr + length);
        arch_flush_tlb();
        tlb_shootdown(virt_to_phys(p->pml4));

        /* Remove any overlapping VMAs in [vaddr, vaddr+length) */
        for (;;) {
            vma_t *ov = vma_find_overlap(p->vma_root, vaddr, vaddr + length);
            if (!ov) break;
            if (ov->start < vaddr && ov->end > vaddr + length) {
                uint64_t orig_end = ov->end;
                int saved_prot = ov->prot;
                int saved_flags = ov->flags;
                uint64_t split_foff = ov->file_ino ? ov->file_offset + (vaddr + length - ov->start) : 0;
                ov->end = vaddr;
                vma_t *sv = vma_insert(&p->vma_root, vaddr + length, orig_end,
                           saved_prot, saved_flags);
                if (sv && ov->file_ino) {
                    sv->file_ino = ov->file_ino;
                    sv->file_offset = split_foff;
                    sv->file_backend = ov->file_backend;
                }
            } else if (ov->start < vaddr) {
                ov->end = vaddr;
            } else if (ov->end > vaddr + length) {
                /* Trim from left: remove + re-insert to keep AVL key order */
                uint64_t new_start = vaddr + length;
                uint64_t ov_end = ov->end;
                int ov_prot = ov->prot;
                int ov_flags = ov->flags;
                uint64_t ov_file_ino = ov->file_ino;
                uint64_t ov_foff = ov->file_ino ? ov->file_offset + (new_start - ov->start) : 0;
                int ov_backend = ov->file_backend;
                vma_remove(&p->vma_root, ov);
                vma_t *nv = vma_insert(&p->vma_root, new_start, ov_end, ov_prot, ov_flags);
                if (nv && ov_file_ino) {
                    nv->file_ino = ov_file_ino;
                    nv->file_offset = ov_foff;
                    nv->file_backend = ov_backend;
                }
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
    if ((flags & MAP_LOCKED) || (p->mlockall_flags & MCL_FUTURE))
        vma_flags |= VMA_LOCKED;
    /* Mark large anonymous mappings as eligible for transparent huge pages */
    if ((flags & MAP_ANONYMOUS) && length >= HUGE_PAGE_SIZE)
        vma_flags |= VMA_HUGEPAGE;
    vma_t *v = vma_insert(&p->vma_root, vaddr, vaddr + length, prot, vma_flags);
    if (!v) {
        spin_unlock_irq(&p->lock, irqf);
        return -ENOMEM;
    }

    /* File-backed mmap: lazy — just store file metadata in VMA.
     * Pages are loaded on demand by the page fault handler. */
    if (is_file) {
        uint64_t ino = vf->disk_ino;
        if (!ino && vf->node) ino = vf->node->ino;
        v->file_ino = ino;
        v->file_offset = (uint64_t)offset;
        v->file_backend = vf->backend;
        spin_unlock_irq(&p->lock, irqf);
        return (long)vaddr;
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

    /* Write-back dirty pages for shared file-backed VMAs before unmapping.
     * Snapshot VMA info under lock, release, write-back, re-acquire. */
    {
        uint64_t irqf_wb;
        spin_lock_irq(&p->lock, &irqf_wb);
        /* Find overlapping shared file-backed VMAs (max 4 for typical use) */
        struct { uint64_t start, end, file_ino, file_offset; int backend; } wb[4];
        int nwb = 0;
        for (uint64_t probe = start; probe < end && nwb < 4; ) {
            vma_t *v = vma_find(p->vma_root, probe);
            if (!v) { probe += 4096; continue; }
            if ((v->flags & VMA_SHARED) && v->file_ino) {
                wb[nwb].start = probe > v->start ? probe : v->start;
                wb[nwb].end = end < v->end ? end : v->end;
                wb[nwb].file_ino = v->file_ino;
                wb[nwb].file_offset = v->file_offset;
                wb[nwb].backend = v->file_backend;
                /* Adjust file_offset for partial VMA overlap */
                if (probe > v->start)
                    wb[nwb].file_offset += probe - v->start;
                nwb++;
            }
            probe = v->end;
        }
        spin_unlock_irq(&p->lock, irqf_wb);
        for (int i = 0; i < nwb; i++) {
            extern long vfs_pwrite_by_ino(int, uint64_t, const void *, size_t, size_t);
            for (uint64_t va = wb[i].start; va < wb[i].end; va += 4096) {
                extern uint64_t read_pte_pub(uint64_t *pml4, uint64_t va);
                uint64_t pte = read_pte_pub(p->pml4, va);
                if ((pte & PTE_PRESENT) && (pte & PTE_DIRTY)) {
                    uint64_t phys = pte & PTE_ADDR_MASK;
                    uint64_t foff = wb[i].file_offset + (va - wb[i].start);
                    vfs_pwrite_by_ino(wb[i].backend, wb[i].file_ino,
                                      phys_to_virt(phys), (size_t)foff, 4096);
                }
            }
        }
    }

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
            uint64_t split_foff = v->file_ino ? v->file_offset + (end - v->start) : 0;
            v->end = start;
            vma_t *rv = vma_insert(&p->vma_root, end, orig_end, v->prot, v->flags);
            if (rv && v->file_ino) {
                rv->file_ino = v->file_ino;
                rv->file_offset = split_foff;
                rv->file_backend = v->file_backend;
            }
            break;
        } else if (v->start < start) {
            v->end = start;
        } else {
            /* Trim from left: remove + re-insert to keep AVL key order */
            uint64_t new_start = end;
            uint64_t v_end = v->end;
            int v_prot = v->prot;
            int v_flags = v->flags;
            uint64_t v_file_ino = v->file_ino;
            uint64_t v_foff = v->file_ino ? v->file_offset + (new_start - v->start) : 0;
            int v_backend = v->file_backend;
            vma_remove(&p->vma_root, v);
            vma_t *nv = vma_insert(&p->vma_root, new_start, v_end, v_prot, v_flags);
            if (nv && v_file_ino) {
                nv->file_ino = v_file_ino;
                nv->file_offset = v_foff;
                nv->file_backend = v_backend;
            }
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
    if (len == 0) return 0;
    if (addr + len < addr || addr + len > 0x800000000000ULL) return -ENOMEM;
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
        vma_t *v = vma_find_overlap(p->vma_root, probe, end);
        if (!v) break; /* no more VMAs in range */

        /* Advance probe past any gap before this VMA */
        if (v->start > probe)
            probe = v->start;

        if (v->start < start) {
            /* Split: left part keeps old prot */
            uint64_t orig_end = v->end;
            int orig_prot = v->prot;
            int orig_flags = v->flags;
            uint64_t split_foff = v->file_ino ? v->file_offset + (start - v->start) : 0;
            v->end = start;
            vma_t *sv = vma_insert(&p->vma_root, start, orig_end, orig_prot, orig_flags);
            if (sv && v->file_ino) {
                sv->file_ino = v->file_ino;
                sv->file_offset = split_foff;
                sv->file_backend = v->file_backend;
            }
            continue; /* re-probe */
        }
        if (v->end > end) {
            /* Split: right part keeps old prot */
            int orig_prot = v->prot;
            int orig_flags = v->flags;
            uint64_t split_foff = v->file_ino ? v->file_offset + (end - v->start) : 0;
            vma_t *sv = vma_insert(&p->vma_root, end, v->end, orig_prot, orig_flags);
            if (sv && v->file_ino) {
                sv->file_ino = v->file_ino;
                sv->file_offset = split_foff;
                sv->file_backend = v->file_backend;
            }
            v->end = end;
        }
        v->prot = prot;
        probe = v->end;
    }

    spin_unlock_irq(&p->lock, irqf);
    return 0;
}

/* ── SYS_madvise (28) ───────────────────────────── */

/* Mark pages as LAZYFREE: set PTE_LAZYFREE, clear Dirty bit.
 * Pages remain mapped and accessible. On next write, CPU sets Dirty,
 * which protects the page from reclaim. */
static void mark_lazyfree_range(uint64_t *user_pml4, uint64_t start, uint64_t end) {
    const uint64_t PHYS_MASK = 0x000FFFFFFFFFF000ULL;
    for (uint64_t va = start; va < end; ) {
        int pml4i = (va >> 39) & 0x1FF;
        if (!(user_pml4[pml4i] & PTE_PRESENT)) {
            va = (va | ((1ULL << 39) - 1)) + 1;
            continue;
        }
        uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & PHYS_MASK);
        int pdpti = (va >> 30) & 0x1FF;
        if (!(pdpt[pdpti] & PTE_PRESENT)) {
            va = (va | ((1ULL << 30) - 1)) + 1;
            continue;
        }
        uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PHYS_MASK);
        int pdi = (va >> 21) & 0x1FF;
        if (!(pd[pdi] & PTE_PRESENT)) {
            va = (va | ((1ULL << 21) - 1)) + 1;
            continue;
        }
        /* Huge page: split before marking individual pages as lazyfree */
        if (pd[pdi] & PTE_PS) {
            if (split_huge_pmd(pd, pdi, 0) < 0) {
                va = (va & HUGE_PAGE_MASK) + HUGE_PAGE_SIZE;
                continue;
            }
        }
        uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PHYS_MASK);
        int pti = (va >> 12) & 0x1FF;
        if (pt[pti] & PTE_PRESENT) {
            /* Set LAZYFREE, clear Dirty (so reclaim knows page is untouched) */
            pt[pti] = (pt[pti] | PTE_LAZYFREE) & ~PTE_DIRTY;
            arch_invlpg(va);
        }
        va += 4096;
    }
}

long do_madvise(unsigned long addr, size_t length, int advice) {
    if (addr & 0xFFF) return -EINVAL;
    length = (length + 0xFFF) & ~0xFFFULL;
    if (length == 0) return 0;
    if (addr + length < addr || addr + length > 0x800000000000ULL) return -EINVAL;

    process_t *p = proc_current();
    if (!p) return -EFAULT;
    uint64_t start = addr;
    uint64_t end = addr + length;

    if (advice == MADV_DONTNEED) {
        /* Drop anonymous pages — next access gets fresh zeros.
         * Only unmap pages in MAP_ANONYMOUS VMAs. File-backed or
         * ELF-loaded pages must not be zeroed (data loss). */
        uint64_t irqf;
        spin_lock_irq(&p->lock, &irqf);
        /* Verify range is within a valid VMA */
        if (!vma_find_overlap(p->vma_root, start, end)) {
            spin_unlock_irq(&p->lock, irqf);
            return -ENOMEM;
        }
        for (uint64_t va = start; va < end; ) {
            vma_t *v = vma_find_overlap(p->vma_root, va, end);
            if (!v) break;
            if (v->start > va) va = v->start;
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
        return 0;
    }

    if (advice == MADV_FREE) {
        /* Lazy reclaim: mark pages as reclaimable but keep them mapped.
         * Pages stay until memory pressure triggers reclaim.
         * Re-write before reclaim clears LAZYFREE (CPU sets Dirty). */
        uint64_t irqf;
        spin_lock_irq(&p->lock, &irqf);
        /* Verify range is within a valid writable anonymous VMA */
        int valid = 0;
        for (uint64_t va = start; va < end; ) {
            vma_t *v = vma_find_overlap(p->vma_root, va, end);
            if (!v) break;
            if (v->start > va) va = v->start;
            if ((v->flags & MAP_ANONYMOUS) && (v->prot & PROT_WRITE)) {
                uint64_t ustart = va > v->start ? va : v->start;
                uint64_t uend = end < v->end ? end : v->end;
                mark_lazyfree_range(p->pml4, ustart, uend);
                valid = 1;
            }
            va = v->end;
        }
        if (!valid && !vma_find_overlap(p->vma_root, start, end)) {
            spin_unlock_irq(&p->lock, irqf);
            return -ENOMEM;
        }
        /* TLB flush to ensure Dirty bit is cleared in all TLBs */
        arch_flush_tlb();
        extern void tlb_shootdown(uint64_t pml4_phys);
        tlb_shootdown(virt_to_phys(p->pml4));
        spin_unlock_irq(&p->lock, irqf);
        return 0;
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

    /* MREMAP_FIXED: move to specific address (requires MREMAP_MAYMOVE) */
    if (flags & MREMAP_FIXED) {
        if (!(flags & MREMAP_MAYMOVE)) {
            spin_unlock_irq(&p->lock, irqf);
            return -EINVAL;
        }
        if (new_addr & 0xFFF) { spin_unlock_irq(&p->lock, irqf); return -EINVAL; }
        if (new_addr + new_size > 0x800000000000ULL) {
            spin_unlock_irq(&p->lock, irqf);
            return -ENOMEM;
        }

        /* Unmap anything at [new_addr, new_addr+new_size) */
        unmap_range(p->pml4, new_addr, new_addr + new_size);
        /* Remove overlapping VMAs */
        for (;;) {
            vma_t *ov = vma_find_overlap(p->vma_root, new_addr, new_addr + new_size);
            if (!ov) break;
            if (ov->start >= new_addr && ov->end <= new_addr + new_size)
                vma_remove(&p->vma_root, ov);
            else if (ov->start < new_addr)
                ov->end = new_addr;
            else {
                uint64_t ns = new_addr + new_size, ne = ov->end;
                int np = ov->prot, nf = ov->flags;
                vma_remove(&p->vma_root, ov);
                vma_insert(&p->vma_root, ns, ne, np, nf);
            }
        }

        int v_prot = v->prot;
        int v_flags = v->flags;
        vma_t *nv = vma_insert(&p->vma_root, new_addr, new_addr + new_size, v_prot, v_flags);
        if (!nv) { spin_unlock_irq(&p->lock, irqf); return -ENOMEM; }
        nv->file_ino = v->file_ino;
        nv->file_offset = v->file_offset;
        nv->file_backend = v->file_backend;

        copy_user_pages(p->pml4, new_addr, old_addr, old_size < new_size ? old_size : new_size, v_prot);
        unmap_range(p->pml4, old_addr, old_addr + old_size);
        arch_flush_tlb();
        tlb_shootdown(virt_to_phys(p->pml4));

        vma_t *old_v = vma_find(p->vma_root, old_addr);
        if (old_v && old_v->start == old_addr)
            vma_remove(&p->vma_root, old_v);

        spin_unlock_irq(&p->lock, irqf);
        return (long)new_addr;
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
    uint64_t v_file_ino = v->file_ino;
    uint64_t v_file_offset = v->file_offset;
    int v_file_backend = v->file_backend;

    /* Create new VMA */
    vma_t *nv = vma_insert(&p->vma_root, new_va, new_va + new_size, v_prot, v_flags);
    if (!nv) { spin_unlock_irq(&p->lock, irqf); return -ENOMEM; }
    nv->file_ino = v_file_ino;
    nv->file_offset = v_file_offset;
    nv->file_backend = v_file_backend;

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

/* ── Dirty page write-back for shared file-backed VMAs ── */

/* Write back dirty pages in [start, end) for a shared file-backed VMA.
 * Caller must NOT hold p->lock (we walk page tables directly). */
static void writeback_dirty_pages(uint64_t *user_pml4, uint64_t start, uint64_t end,
                                  int file_backend, uint64_t file_ino,
                                  uint64_t file_offset, uint64_t vma_start) {
    extern long vfs_pwrite_by_ino(int, uint64_t, const void *, size_t, size_t);
    for (uint64_t va = start; va < end; va += 4096) {
        int pml4i = (va >> 39) & 0x1FF;
        if (!(user_pml4[pml4i] & PTE_PRESENT)) {
            va = (va | ((1ULL << 39) - 1));
            continue;
        }
        uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & PTE_ADDR_MASK);
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
        if (pd[pdi] & PTE_PS) continue; /* huge pages not used for file-backed */
        uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PTE_ADDR_MASK);
        int pti = (va >> 12) & 0x1FF;
        uint64_t pte = pt[pti];
        if ((pte & PTE_PRESENT) && (pte & PTE_DIRTY)) {
            uint64_t phys = pte & PTE_ADDR_MASK;
            uint64_t foff = file_offset + (va - vma_start);
            vfs_pwrite_by_ino(file_backend, file_ino,
                              phys_to_virt(phys), (size_t)foff, 4096);
            /* Clear dirty bit — page is now clean */
            pt[pti] = pte & ~PTE_DIRTY;
            arch_invlpg(va);
        }
    }
}

/* ── SYS_msync (26) ──────────────────────────────── */

long do_msync(unsigned long addr, size_t length, int flags) {
    if (addr & 0xFFF) return -EINVAL;
    if (length == 0) return -EINVAL;
    int valid = MS_ASYNC | MS_SYNC | MS_INVALIDATE;
    if (flags & ~valid) return -EINVAL;
    if ((flags & MS_ASYNC) && (flags & MS_SYNC)) return -EINVAL;
    if (!(flags & (MS_ASYNC | MS_SYNC))) return -EINVAL;

    process_t *p = proc_current();
    if (!p) return -EFAULT;

    length = (length + 0xFFF) & ~0xFFFULL;
    uint64_t start = addr;
    uint64_t end = addr + length;

    /* Walk pages in the range, find shared file-backed VMAs, write dirty pages */
    for (uint64_t va = start; va < end; ) {
        uint64_t irqf;
        spin_lock_irq(&p->lock, &irqf);
        vma_t *vma = vma_find(p->vma_root, va);
        if (!vma) {
            spin_unlock_irq(&p->lock, irqf);
            /* Skip to next page — no VMA at this address */
            va += 4096;
            continue;
        }
        /* Snapshot VMA fields under lock */
        uint64_t v_start = vma->start;
        uint64_t v_end = vma->end;
        int v_flags = vma->flags;
        uint64_t v_file_ino = vma->file_ino;
        uint64_t v_file_offset = vma->file_offset;
        int v_file_backend = vma->file_backend;
        spin_unlock_irq(&p->lock, irqf);

        /* Only write-back shared file-backed VMAs */
        if ((v_flags & VMA_SHARED) && v_file_ino) {
            uint64_t wb_start = va > v_start ? va : v_start;
            uint64_t wb_end = end < v_end ? end : v_end;
            writeback_dirty_pages(p->pml4, wb_start, wb_end,
                                  v_file_backend, v_file_ino,
                                  v_file_offset, v_start);
        }
        va = v_end; /* advance past this VMA */
    }
    return 0;
}
