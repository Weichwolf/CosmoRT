/* CosmoRT Interrupt handling — APIC + IDT, thread-aware */

#include "core/irq.h"
#include "core/frame.h"
#include "hw/serial.h"
#include "proc/thread.h"
#include "proc/process.h"
#include "core/percpu.h"
#include "sys/syscall.h"
#include "config.h"
#include "mm/vma.h"
#include "mm/page_alloc.h"
#include "mm/page_cache.h"
#include "core/smp.h"
#include "spinlock.h"
#include "arch/arch.h"
#include "hal/hal.h"
#include "memops.h"
#include "mm/extable.h"
#include "core/tick.h"

/* ── Local APIC ────────────────────────────────────── */

#define LAPIC_PHYS       0xFEE00000
#define LAPIC_BASE       (LAPIC_PHYS + PHYS_OFFSET)
#define LAPIC_EOI        0x0B0
#define LAPIC_SVR        0x0F0
#define LAPIC_TPR        0x080
#define LAPIC_TIMER      0x320
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_DIV  0x3E0

static volatile uint32_t *lapic = (volatile uint32_t *)LAPIC_BASE;

static void lapic_write(uint32_t reg, uint32_t val) { lapic[reg/4] = val; }

void lapic_eoi(void) { lapic_write(LAPIC_EOI, 0); }

/* ── I/O APIC ──────────────────────────────────────── */

#define IOAPIC_PHYS 0xFEC00000
#define IOAPIC_BASE (IOAPIC_PHYS + PHYS_OFFSET)

static volatile uint32_t *ioapic = (volatile uint32_t *)IOAPIC_BASE;

static void ioapic_write(uint8_t reg, uint32_t val) {
    ioapic[0] = reg;
    ioapic[4] = val;
}

static uint32_t ioapic_read(uint8_t reg) {
    ioapic[0] = reg;
    return ioapic[4];
}

void ioapic_route_irq(uint8_t irq, uint8_t vector) {
    ioapic_write(0x10 + irq*2, vector);
    ioapic_write(0x10 + irq*2 + 1, 0);
}

/* Level-triggered, active-low — required for PCI INTx shared IRQs */
void ioapic_route_irq_level(uint8_t irq, uint8_t vector) {
    /* Bit 13 = active-low, bit 15 = level-triggered */
    ioapic_write(0x10 + irq*2, (uint32_t)vector | (1 << 13) | (1 << 15));
    ioapic_write(0x10 + irq*2 + 1, 0);
}

/* ── IDT ───────────────────────────────────────────── */

struct idt_entry {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256] __attribute__((aligned(16)));
static struct idt_ptr idtp;

extern uint64_t isr_stub_table[256];

static void idt_set_entry(int n, uint64_t handler) {
    idt[n].offset_lo  = handler & 0xFFFF;
    idt[n].selector   = 0x08;
    idt[n].ist        = 0;
    idt[n].type_attr  = 0x8E;
    idt[n].offset_mid = (handler >> 16) & 0xFFFF;
    idt[n].offset_hi  = (handler >> 32) & 0xFFFFFFFF;
    idt[n].reserved   = 0;
}

static void idt_set_entry_user(int n, uint64_t handler) {
    idt[n].offset_lo  = handler & 0xFFFF;
    idt[n].selector   = 0x08;
    idt[n].ist        = 0;
    idt[n].type_attr  = 0xEF;
    idt[n].offset_mid = (handler >> 16) & 0xFFFF;
    idt[n].offset_hi  = (handler >> 32) & 0xFFFFFFFF;
    idt[n].reserved   = 0;
}

/* ── Helpers ───────────────────────────────────────── */

/* Forward declaration */
static void default_exception_with_frame(int vector, irq_frame_t *frame);

/* ── Cold-path error helpers (keep strings out of hot IRQ dispatch) ── */

__attribute__((cold, noreturn))
static void pf_kernel_panic(uint64_t cr2, uint64_t error, uint64_t rip) {
    serial_puts("\nPAGE FAULT in kernel CR2=");
    serial_hex64(cr2);
    serial_puts(" err=");
    serial_hex64(error);
    serial_puts(" rip=");
    serial_hex64(rip);
    serial_puts(" KERNEL PANIC\n");
    hal_cpu_halt_noirq();
    __builtin_unreachable();
}

__attribute__((cold))
static void segfault_log(uint64_t cr2, uint64_t rip, uint64_t error, int pid) {
    serial_puts("\nSEGFAULT CR2=");
    serial_hex64(cr2);
    serial_puts(" RIP=");
    serial_hex64(rip);
    serial_puts(" err=");
    serial_hex64(error);
    if (pid >= 0) {
        serial_puts(" pid=");
        serial_hex64((uint64_t)(unsigned)pid);
        serial_puts(" killed\n");
    }
}

/* ── Handlers ──────────────────────────────────────── */

#define MAX_HANDLERS 256
static irq_handler_t handlers[MAX_HANDLERS];
volatile int irq_event_pending = 0;

void irq_register(int vector, irq_handler_t handler) {
    if (vector >= 0 && vector < MAX_HANDLERS)
        handlers[vector] = handler;
}

__attribute__((hot))
void irq_dispatch(int vector, irq_frame_t *frame) {
    /* INT 0x80: syscall from Ring 3 (legacy path) */
    if (vector == 0x80) {
        long sysnum = (long)frame->rax;
        frame->rax = (uint64_t)sys_handler(
            sysnum, (long)frame->rdi, (long)frame->rsi,
            (long)frame->rdx, (long)frame->r10, (long)frame->r8,
            (long)frame->r9);
        /* Signal delivery for INT 0x80 path: sync irq_frame ↔ thread_t */
        if (sysnum != 15 /* SYS_RT_SIGRETURN */) {
            extern void check_pending_signals(void);
            thread_t *t = percpu_self()->current_thread;
            if (t && t->proc && (t->proc->sig_pending & ~t->sig_blocked)) {
                irq_frame_to_thread(frame, t);
                check_pending_signals();
                thread_to_irq_frame(t, frame);
            }
        }
        return;
    }

    /* Page fault (vector 14): demand paging */
    if (vector == 14) {
        uint64_t cr2 = hal_mmu_fault_address();
        uint64_t error = frame->error;

        /* Kernel-mode fault (bit 2 = 0): try demand paging for user addresses */
        if (!(error & 4)) {
            /* If faulting address is in user half, the kernel was accessing
             * a valid-but-unmapped user buffer (e.g. copy_path_from_user).
             * Look up VMA, demand-map if possible, resume. */
            if (cr2 < 0x800000000000ULL) {
                percpu_t *kcpu = percpu_self();
                thread_t *kt = kcpu->current_thread;
                process_t *kp = kt ? kt->proc : 0;
                if (kp) {
                    uint64_t kflags;
                    if (spin_trylock_irq(&kp->lock, &kflags)) {
                        vma_t *kvma = vma_find(kp->vma_root, cr2);
                        int kprot = kvma ? kvma->prot : 0;
                        /* PROT_NONE VMAs must never be demand-paged — access
                         * from kernel context (e.g. copy_*_user on a bad
                         * pointer) falls through to extable/legacy recovery. */
                        int knp = kvma && !(error & 1) &&
                                  (kprot & (PROT_READ | PROT_WRITE | PROT_EXEC));
                        int kcow = kvma && (error & 1) && (error & 2) && (kprot & PROT_WRITE);
                        uint64_t k_file_ino = kvma ? kvma->file_ino : 0;
                        uint64_t k_file_offset = kvma ? kvma->file_offset : 0;
                        int k_file_backend = kvma ? kvma->file_backend : 0;
                        int k_shared = kvma ? (kvma->flags & VMA_SHARED) : 0;
                        uint64_t k_vma_start = kvma ? kvma->start : 0;
                        spin_unlock_irq(&kp->lock, kflags);
                        if (knp) {
                            uint64_t kpage_addr = cr2 & ~0xFFFULL;
                            if (k_file_ino) {
                                /* File-backed demand page in kernel access path.
                                 * Page-cache shares one phys across all consumers
                                 * (MAP_SHARED + MAP_PRIVATE). MAP_PRIVATE writers
                                 * trip the COW path on first store. */
                                uint64_t foff = k_file_offset + (kpage_addr - k_vma_start);
                                uint64_t phys = page_cache_get_or_load(k_file_backend,
                                                                        k_file_ino, foff);
                                if (phys) {
                                    /* MAP_SHARED writable → drop WRITE for dirty tracking.
                                     * MAP_PRIVATE writable → drop WRITE + set COW so first
                                     * store forks a private copy. */
                                    int kmap_prot = kprot;
                                    if (kprot & PROT_WRITE) kmap_prot = kprot & ~PROT_WRITE;
                                    if (map_user_page(kp->pml4, kpage_addr, phys, kmap_prot) == 0) {
                                        if (!k_shared && (kprot & PROT_WRITE)) {
                                            /* Mark PTE_COW for private writable */
                                            extern void pte_or_inplace(uint64_t *pml4, uint64_t va, uint64_t bits);
                                            pte_or_inplace(kp->pml4, kpage_addr, (1ULL << 9));
                                        }
                                        return;
                                    }
                                    page_free(phys_to_virt(phys));
                                }
                            } else {
                                uint64_t *kpage = alloc_page();
                                if (kpage) {
                                    if (map_user_page(kp->pml4, kpage_addr,
                                                      virt_to_phys(kpage), kprot) == 0) {
                                        return; /* resume kernel code */
                                    }
                                    page_free(kpage);
                                }
                            }
                        }
                        /* Kernel write to COW user page */
                        if (kcow) {
                            uint64_t kpage_addr = cr2 & ~0xFFFULL;
                            extern uint64_t read_pte_pub(uint64_t *pml4, uint64_t va);
                            uint64_t kpte = read_pte_pub(kp->pml4, kpage_addr);
                            #define PTE_COW_K (1ULL << 9)
                            if ((kpte & PTE_COW_K) && (kpte & 1)) {
                                uint64_t old_phys = kpte & 0x000FFFFFFFFFF000ULL;
                                int krc = page_refcount(old_phys);
                                if (krc <= 1) {
                                    if (map_user_page(kp->pml4, kpage_addr, old_phys, kprot) == 0) {
                                        hal_mmu_flush(kpage_addr);
                                        return;
                                    }
                                } else {
                                    uint64_t *knew = alloc_page();
                                    if (knew) {
                                        kmemcpy(knew, phys_to_virt(old_phys), 4096);
                                        if (map_user_page(kp->pml4, kpage_addr,
                                                          virt_to_phys(knew), kprot) == 0) {
                                            hal_mmu_flush(kpage_addr);
                                            page_free(phys_to_virt(old_phys));
                                            return;
                                        }
                                        page_free(knew);
                                    }
                                }
                            }
                        }
                    }
                    /* trylock failed → fall through to extable */
                }
            }
            /* Kernel accessed unmapped user address (e.g. bad pointer from
             * syscall). Exception table maps each inline-asm user-access
             * instruction to its fixup. On hit, only RIP is rewritten —
             * no register/stack/FPU touch. */
            if (cr2 < 0x800000000000ULL) {
                uintptr_t fixup = extable_lookup((uintptr_t)frame->rip);
                if (fixup) {
                    frame->rip = (uint64_t)fixup;
                    return;
                }
            }
            /* Kernel fault: kill process if in syscall, skip if in IRQ */
            {
                thread_t *kft2 = percpu_self()->current_thread;
                if (kft2 && kft2->proc && kft2->proc->pml4) {
                    serial_puts("\nKERNEL PF → kill pid=");
                    serial_hex64(kft2->proc->pid);
                    serial_puts(" cr2="); serial_hex64(cr2);
                    serial_puts(" rip="); serial_hex64(frame->rip);
                    {
                        extern void do_exit(int);
                        serial_puts(" do_exit="); serial_hex64((uint64_t)(uintptr_t)do_exit);
                    }
                    serial_puts(" rdi="); serial_hex64(frame->rdi);
                    serial_puts(" rdx="); serial_hex64(frame->rdx);
                    serial_putchar('\n');
                    extern void do_exit_group(int status);
                    extern uint64_t pml4[];
                    hal_mmu_switch(virt_to_phys(pml4));
                    kft2->proc->exit_signal = 11;
                    do_exit_group(128 + 11);
                }
                /* NULL call in IRQ/idle context (stale timer callback):
                 * pop return address from stack, resume caller. */
                if (frame->rip == 0) {
                    serial_puts("\nKERNEL NULL-CALL in IRQ → skip\n");
                    frame->rip = *(uint64_t *)frame->rsp;
                    frame->rsp += 8;
                    return;
                }
            }
            pf_kernel_panic(cr2, error, frame->rip);
        }

        /* User-mode page fault — try demand paging */
        percpu_t *cpu = percpu_self();
        thread_t *t = cpu->current_thread;
        process_t *p = t ? t->proc : 0;

        if (p) {
            uint64_t vma_flags;
            spin_lock_irq(&p->lock, &vma_flags);
            vma_t *vma = vma_find(p->vma_root, cr2);
            if (vma) {
                /* PROT_NONE VMA (e.g. stack guard page) → deterministic SIGSEGV */
                if ((vma->prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0) {
                    spin_unlock_irq(&p->lock, vma_flags);
                    goto kill_process;
                }
                /* Protection violation: check write permission */
                if ((error & 1) && (error & 2) && !(vma->prot & PROT_WRITE)) {
                    /* Write to read-only VMA → kill */
                    spin_unlock_irq(&p->lock, vma_flags);
                    goto kill_process;
                }
                /* COW fault: write to present page with COW bit in writable VMA */
                if ((error & 1) && (error & 2) && (vma->prot & PROT_WRITE)) {
                    int cow_prot = vma->prot;
                    spin_unlock_irq(&p->lock, vma_flags);
                    uint64_t page_addr = cr2 & ~0xFFFULL;
                    extern uint64_t read_pte_pub(uint64_t *pml4, uint64_t va);
                    uint64_t pte = read_pte_pub(p->pml4, page_addr);
                    #define PTE_COW_IRQ (1ULL << 9)
                    /* Write to writable VMA but page not COW: re-map with correct prot.
                     * This handles mprotect race (PTE stale after VMA prot change). */
                    if ((pte & 1) && !(pte & PTE_COW_IRQ)) {
                        uint64_t phys = pte & 0x000FFFFFFFFFF000ULL;
                        if (map_user_page(p->pml4, page_addr, phys, cow_prot) == 0) {
                            hal_mmu_flush(page_addr);
                            return;
                        }
                        goto kill_process;
                    }
                    if ((pte & PTE_COW_IRQ) && (pte & 1)) {
                        uint64_t old_phys = pte & 0x000FFFFFFFFFF000ULL;
                        int rc = page_refcount(old_phys);
                        if (rc <= 1) {
                            /* Last reference: just restore write + remove COW */
                            if (map_user_page(p->pml4, page_addr, old_phys, cow_prot) == 0) {
                                hal_mmu_flush(page_addr);
                                return;
                            }
                        } else {
                            /* Shared: copy page, release our ref on old */
                            uint64_t *new_page = alloc_page();
                            if (new_page) {
                                kmemcpy(new_page, phys_to_virt(old_phys), 4096);
                                if (map_user_page(p->pml4, page_addr,
                                                  virt_to_phys(new_page), cow_prot) == 0) {
                                    hal_mmu_flush(page_addr);
                                    /* Notify dedup: old hash invalidated, new page diverged */
                                    /* page_free handles refcount: decrements and
                                     * only returns to buddy when count reaches 0 */
                                    page_free(phys_to_virt(old_phys));
                                    return;
                                }
                                page_free(new_page);
                            }
                        }
                    }
                    goto kill_process;
                }
                /* NX violation: page present + instruction fetch, but VMA allows exec.
                 * Re-map the page with correct permissions (PTE stale from mprotect race). */
                if ((error & 1) && (error & 0x10) && (vma->prot & PROT_EXEC)) {
                    int nx_prot = vma->prot;
                    spin_unlock_irq(&p->lock, vma_flags);
                    uint64_t page_addr = cr2 & ~0xFFFULL;
                    extern uint64_t read_pte_pub(uint64_t *pml4, uint64_t va);
                    uint64_t pte = read_pte_pub(p->pml4, page_addr);
                    if (pte & 1) {
                        uint64_t phys = pte & 0x000FFFFFFFFFF000ULL;
                        if (map_user_page(p->pml4, page_addr, phys, nx_prot) == 0) {
                            hal_mmu_flush(page_addr);
                            return; /* resume execution */
                        }
                    }
                    goto kill_process;
                }
                /* Not-present fault in a valid VMA → allocate page.
                 * PROT_NONE VMAs must NOT be demand-paged — access = SIGSEGV. */
                if (!(error & 1) && (vma->prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) {
                    int dp_prot = vma->prot;
                    int dp_huge = (vma->flags & VMA_HUGEPAGE) && !(vma->flags & VMA_NOHUGEPAGE);
                    uint64_t vma_start = vma->start;
                    uint64_t vma_end = vma->end;
                    uint64_t dp_file_ino = vma->file_ino;
                    uint64_t dp_file_offset = vma->file_offset;
                    int dp_file_backend = vma->file_backend;
                    int dp_shared = (vma->flags & VMA_SHARED);
                    spin_unlock_irq(&p->lock, vma_flags);
                    uint64_t page_addr = cr2 & ~0xFFFULL;

                    /* File-backed demand paging — page cache shared across
                     * processes for both MAP_SHARED and MAP_PRIVATE.
                     * MAP_PRIVATE writers go through COW on first store. */
                    if (__builtin_expect(dp_file_ino != 0, 0)) {
                        extern void pte_or_inplace(uint64_t *pml4, uint64_t va, uint64_t bits);
                        uint64_t foff = dp_file_offset + (page_addr - vma_start);
                        uint64_t phys = page_cache_get_or_load(dp_file_backend,
                                                                dp_file_ino, foff);
                        if (!phys) goto kill_process;
                        /* Writable VMA → drop WRITE bit. MAP_SHARED:
                         * dirty-tracking via second fault. MAP_PRIVATE:
                         * COW via second fault (PTE_COW set after install). */
                        int map_prot = dp_prot;
                        if (dp_prot & PROT_WRITE) map_prot = dp_prot & ~PROT_WRITE;
                        if (map_user_page(p->pml4, page_addr, phys, map_prot) != 0) {
                            page_free(phys_to_virt(phys));
                            goto kill_process;
                        }
                        if (!dp_shared && (dp_prot & PROT_WRITE))
                            pte_or_inplace(p->pml4, page_addr, (1ULL << 9));
                        /* Read-ahead: synchronously load + map up to RA_WINDOW
                         * additional pages forward. Bounded by VMA end. Each
                         * extra page is a cache hit (page_cache_get_or_load
                         * keeps deduping) so this only pays the cost of
                         * walking page tables and incref. Skips PTEs already
                         * present (e.g. earlier readahead from a sibling). */
                        #define RA_WINDOW 16
                        extern uint64_t read_pte_pub(uint64_t *pml4, uint64_t va);
                        for (int ra = 1; ra < RA_WINDOW; ra++) {
                            uint64_t ra_va = page_addr + (uint64_t)ra * 4096;
                            if (ra_va >= vma_end) break;
                            if (read_pte_pub(p->pml4, ra_va) & 1) continue;
                            uint64_t ra_off = foff + (uint64_t)ra * 4096;
                            uint64_t ra_phys = page_cache_get_or_load(dp_file_backend,
                                                                       dp_file_ino, ra_off);
                            if (!ra_phys) break; /* OOM / I/O — stop, fault will retry */
                            if (map_user_page(p->pml4, ra_va, ra_phys, map_prot) != 0) {
                                page_free(phys_to_virt(ra_phys));
                                break;
                            }
                            if (!dp_shared && (dp_prot & PROT_WRITE))
                                pte_or_inplace(p->pml4, ra_va, (1ULL << 9));
                            extern volatile uint64_t pc_stat_ra;
                            __atomic_fetch_add(&pc_stat_ra, 1, __ATOMIC_RELAXED);
                        }
                        return;
                    }

                    /* Anonymous: try 2MB huge page if VMA is eligible and aligned */
                    if (dp_huge) {
                        uint64_t huge_base = cr2 & ~0x1FFFFFULL;
                        if (huge_base >= vma_start &&
                            huge_base + 0x200000ULL <= vma_end) {
                            void *hp = huge_page_alloc();
                            if (hp) {
                                if (map_user_huge_page(p->pml4, huge_base,
                                                       virt_to_phys(hp), dp_prot) == 0) {
                                    return; /* resume execution */
                                }
                                huge_page_free(hp);
                            }
                            /* Fallback to 4KB */
                        }
                    }

                    uint64_t *page = alloc_page();
                    if (page) {
                        if (map_user_page(p->pml4, page_addr,
                                          virt_to_phys(page), dp_prot) == 0) {
                            extern volatile uint64_t pc_stat_anon;
                            __atomic_fetch_add(&pc_stat_anon, 1, __ATOMIC_RELAXED);
                            return; /* resume execution */
                        }
                        page_free(page);
                    }
                    goto kill_process;
                }
                /* Unhandled fault within a valid VMA (e.g. present+NX without exec prot) */
                spin_unlock_irq(&p->lock, vma_flags);
                goto kill_process;
            } else {
                /* No VMA at fault address — check for stack growth.
                 * If a VMA_GROWSDOWN VMA starts just above cr2,
                 * expand it downward (like Linux expand_downwards). */
                vma_t *above = vma_find_above(p->vma_root, cr2);
                if (above && (above->flags & VMA_GROWSDOWN) && !(error & 1)) {
                    /* Stack limit: RLIMIT_STACK (default 8MB) minus 1MB guard gap */
                    unsigned long stack_limit = p->rlim_stack ? p->rlim_stack : RLIM_STACK_DEFAULT;
                    uint64_t min_addr = p->stack_top - stack_limit;
                    uint64_t fault_page = cr2 & ~0xFFFULL;

                    /* Check: within stack limit and not colliding with adjacent VMA */
                    if (fault_page >= min_addr && fault_page < above->start) {
                        /* Stack gap: ensure at least 1MB between stack and next lower VMA */
                        vma_t *below = vma_find(p->vma_root, fault_page);
                        if (!below) {
                            /* Check for VMA ending just below or overlapping */
                            vma_t *prev = vma_find_overlap(p->vma_root,
                                            fault_page > STACK_GUARD_GAP ?
                                            fault_page - STACK_GUARD_GAP : 0,
                                            fault_page);
                            if (prev && prev != above &&
                                fault_page - prev->end < STACK_GUARD_GAP) {
                                /* Too close to adjacent VMA — no growth */
                                spin_unlock_irq(&p->lock, vma_flags);
                                goto kill_process;
                            }
                            /* Expand: move VMA start down to fault page.
                             * Remove + re-insert to keep AVL tree sorted. */
                            uint64_t new_start = fault_page;
                            int sp_prot = above->prot;
                            int sp_flags = above->flags;
                            uint64_t sp_end = above->end;
                            vma_remove(&p->vma_root, above);
                            vma_t *grown = vma_insert(&p->vma_root,
                                                      new_start, sp_end,
                                                      sp_prot, sp_flags);
                            spin_unlock_irq(&p->lock, vma_flags);
                            if (grown) {
                                /* Demand-page the faulting page */
                                uint64_t *page = alloc_page();
                                if (page) {
                                    if (map_user_page(p->pml4, fault_page,
                                                      virt_to_phys(page), sp_prot) == 0)
                                        return;
                                    page_free(page);
                                }
                            }
                            goto kill_process;
                        }
                    }
                }
                spin_unlock_irq(&p->lock, vma_flags);
            }
        }

    kill_process:
        /* Try to deliver SIGSEGV to user handler before killing */
        if (t && t->proc) {
            process_t *faultp = t->proc;
            struct k_sigaction *sa = &faultp->sig_actions[11]; /* SIGSEGV=11 */
            if ((uint64_t)sa->sa_handler > 1 && !(t->sig_blocked & SIG_BIT(11))) {
                /* User SIGSEGV handler registered and not blocked — deliver signal. */
                irq_frame_to_thread(frame, t);
                t->fault_addr = cr2;

                extern void deliver_signal(thread_t *t, int signo);
                deliver_signal(t, 11);

                thread_to_irq_frame(t, frame);
                return; /* resume into signal handler */
            }
        }

        segfault_log(cr2, frame->rip, error, (t && t->proc) ? (int)t->proc->pid : -1);
        if (t && t->proc) {
            extern void do_exit_group(int status);
            hal_mmu_switch(virt_to_phys(t->proc->pml4));
            t->proc->exit_signal = 11;
            do_exit_group(128 + 11); /* SIGSEGV */
        }
        /* Last resort: if NULL call, try to skip */
        if (frame->rip == 0 && (error & 0x10)) {
            serial_puts(" NULL-CALL in kernel (no process) → skip\n");
            frame->rip = *(uint64_t *)frame->rsp;
            frame->rsp += 8;
            return;
        }
        pf_kernel_panic(cr2, error, frame->rip);
    }

    /* Other CPU exceptions (0-31, except 14 handled above): use frame-aware handler */
    if (vector < 32 && vector != 14) {
        default_exception_with_frame(vector, frame);
        return;
    }

    /* Timer (vector 32): handler (tick_run + I/O-polling) laeuft zuerst,
     * damit sched_preempt frisch gesetzte sig_pending/alarm sieht. */
    if (vector == 32 && vector < MAX_HANDLERS && handlers[vector]) {
        handlers[vector](vector);
        extern void sched_preempt(irq_frame_t *frame);
        sched_preempt(frame);
        irq_event_pending = 1;
        lapic_eoi();
        return;
    }

    /* Reschedule IPI (vector 0xFD): preempt immediately instead of
     * waiting for next timer tick. Gives <1ms wake-up latency. */
    if (vector == 0xFD) {
        extern void sched_preempt(irq_frame_t *frame);
        sched_preempt(frame);
    }

    if (vector < MAX_HANDLERS && handlers[vector])
        handlers[vector](vector);
    irq_event_pending = 1;
    lapic_eoi();
}

/* Exception handler — kills user thread, halts on kernel fault */

__attribute__((cold))
static void default_exception_with_frame(int vector, irq_frame_t *frame) {
    percpu_t *cpu = percpu_self();
    thread_t *t = cpu->current_thread;

    /* Kernel-mode exception during syscall: the kernel faulted on user-
     * supplied data (e.g. fxrstor with garbage). Extable maps the faulting
     * instruction to its fixup — pure RIP rewrite, no register touch. */
    if (!(frame->cs & 3)) {
        uintptr_t fixup = extable_lookup((uintptr_t)frame->rip);
        if (fixup) {
            frame->rip = (uint64_t)fixup;
            return;
        }
    }

    /* User-mode exception: try to deliver as signal before killing.
     * Map: GPF(13), Page Fault(14) → SIGSEGV(11);
     *      INT3(3) → SIGTRAP(5); FPE(0,16,19) → SIGFPE(8) */
    if (t && t->proc && (frame->cs & 3)) {
        int signo = 0;
        if (vector == 13 || vector == 14) signo = SIGSEGV;
        else if (vector == 3) signo = SIGTRAP;
        else if (vector == 0 || vector == 16 || vector == 19) signo = SIGFPE;
        else if (vector == 6) signo = SIGILL;

        if (signo) {
            struct k_sigaction *sa = &t->proc->sig_actions[signo];
            /* Only deliver if handler exists AND signal not already blocked
             * (blocked = we're already in the handler → avoid infinite loop) */
            if ((uint64_t)sa->sa_handler > 1 && !(t->sig_blocked & SIG_BIT(signo))) {
                irq_frame_to_thread(frame, t);
                /* PF: fault addr = CR2. Other traps: fault addr = faulting RIP. */
                t->fault_addr = (vector == 14) ? hal_mmu_fault_address() : frame->rip;

                extern void deliver_signal(thread_t *t, int signo);
                deliver_signal(t, signo);

                thread_to_irq_frame(t, frame);
                return; /* resume into signal handler */
            }
        }
    }

    serial_puts("\nEXCEPTION ");
    serial_putchar('0' + vector / 10);
    serial_putchar('0' + vector % 10);
    serial_puts(" rip=");
    serial_hex64(frame->rip);
    serial_puts(" cs=");
    serial_hex64(frame->cs);
    serial_puts(" err=");
    serial_hex64(frame->error);
    serial_puts(" rsp=");
    serial_hex64(frame->rsp);
    /* Kernel-base reference: print do_exit's runtime address.
     * Caller subtracts symbol's file-offset (nm) from this to recover
     * the load-time kernel base, then maps any kernel-mode rip back to
     * a source line. */
    {
        extern void do_exit(int status);
        serial_puts(" do_exit=");
        serial_hex64((uint64_t)(uintptr_t)do_exit);
    }

    if (vector == 14) {
        uint64_t cr2 = hal_mmu_fault_address();
        serial_puts(" CR2="); serial_hex64(cr2);
    }

    if (t && t->proc) {
        serial_puts(" pid=");
        serial_hex64(t->proc->pid);
        serial_puts(" tid=");
        serial_hex64(t->tid);
        serial_puts("\n  rax="); serial_hex64(frame->rax);
        serial_puts(" rbx="); serial_hex64(frame->rbx);
        serial_puts(" rcx="); serial_hex64(frame->rcx);
        serial_puts(" rdx="); serial_hex64(frame->rdx);
        serial_puts("\n  rsi="); serial_hex64(frame->rsi);
        serial_puts(" rdi="); serial_hex64(frame->rdi);
        serial_puts(" rbp="); serial_hex64(frame->rbp);
        serial_puts(" r8=");  serial_hex64(frame->r8);
        serial_puts("\n  r9=");  serial_hex64(frame->r9);
        serial_puts(" r10="); serial_hex64(frame->r10);
        serial_puts(" r11="); serial_hex64(frame->r11);
        serial_puts(" r12="); serial_hex64(frame->r12);
        serial_puts("\n  r13="); serial_hex64(frame->r13);
        serial_puts(" r14="); serial_hex64(frame->r14);
        serial_puts(" r15="); serial_hex64(frame->r15);
        serial_puts(" fs=");  serial_hex64(hal_cpu_get_tls());
        serial_puts(" killed\n");
        /* Use do_exit_group to properly close FDs, wake parent, etc. */
        extern void do_exit_group(int status);
        hal_mmu_switch(virt_to_phys(t->proc->pml4));
        t->proc->exit_signal = vector;
        do_exit_group(128 + vector);
    }

    /* Kernel fault in syscall context: kill process instead of panic */
    if (t && t != (thread_t *)(void *)&t /* not idle */ && t->proc && t->proc->pml4) {
        serial_puts(" KERNEL FAULT in syscall → kill pid=");
        serial_hex64(t->proc->pid);
        serial_putchar('\n');
        extern void do_exit_group(int status);
        extern uint64_t pml4[];
        hal_mmu_switch(virt_to_phys(pml4));
        t->proc->exit_signal = vector;
        do_exit_group(128 + vector);
    }

    /* Kernel fault in IRQ/idle context: try to skip and continue.
     * This handles stale TCP timer callbacks calling freed connections.
     * The IRQ frame's RIP is patched to skip the faulting call. */
    if (frame->rip == 0 && frame->cs == 0x08) {
        serial_puts(" KERNEL NULL-CALL — skipping (stale callback?)\n");
        /* Return address is on the stack at RSP. Pop it into RIP. */
        frame->rip = *(uint64_t *)frame->rsp;
        frame->rsp += 8;
        return;
    }

    serial_puts(" KERNEL PANIC\n");
    hal_cpu_halt_noirq();
}

__attribute__((cold))
static void default_exception(int vector) {
    /* Legacy path without frame — shouldn't be called for exceptions
     * that go through irq_dispatch with frame, but kept as fallback */
    serial_puts("\nEXCEPTION ");
    serial_putchar('0' + vector / 10);
    serial_putchar('0' + vector % 10);
    serial_puts(" KERNEL PANIC\n");
    hal_cpu_halt_noirq();
}

/* ── TLB Shootdown IPI ─────────────────────────────── */

static volatile uint64_t shootdown_pml4 = 0;
static volatile int shootdown_ack = 0;
static spinlock_t shootdown_lock = SPINLOCK_INIT;

static void tlb_shootdown_handler(int vector) {
    (void)vector;
    /* Flush TLB if this core uses the affected address space, or if
     * pml4==0 (unconditional flush, e.g. from free_address_space). */
    uint64_t target = shootdown_pml4;
    if (target == 0) {
        hal_mmu_flush_all();
    } else {
        percpu_t *cpu = percpu_self();
        thread_t *t = cpu->current_thread;
        if (t && t->proc && virt_to_phys(t->proc->pml4) == target)
            hal_mmu_flush_all();
    }
    __sync_fetch_and_add(&shootdown_ack, 1);
    /* EOI handled by irq_dispatch — do NOT double-EOI */
}

void tlb_shootdown(uint64_t pml4_phys) {
    int ncores = smp_num_cores();
    if (ncores < 2) return; /* single core, local invlpg suffices */

    uint64_t flags;
    spin_lock_irq(&shootdown_lock, &flags);

    shootdown_ack = 0;
    shootdown_pml4 = pml4_phys;
    hal_cpu_mfence();

    /* Send IPI to all other cores: all-excluding-self shorthand, vector 0xFE */
    volatile uint32_t *icr_lo = (volatile uint32_t *)(LAPIC_BASE + 0x300);
    *icr_lo = 0x000C0000 | 0xFE;

    /* Wait for all other cores to ACK (with timeout to avoid deadlock) */
    int expected = ncores - 1;
    for (int i = 0; i < 10000000; i++) {
        if (__sync_val_compare_and_swap(&shootdown_ack, expected, expected) >= expected)
            break;
        hal_cpu_relax();
    }

    spin_unlock_irq(&shootdown_lock, flags);
}

/* ── Timer ─────────────────────────────────────────── */

static volatile uint64_t tick_count = 0;

static void timer_handler(int vector) {
    (void)vector;
    uint64_t ticks = __sync_add_and_fetch(&tick_count, 1);
    extern void random_add_interrupt_entropy(void);
    random_add_interrupt_entropy();
    tick_run(ticks * TICK_NS_PER_MS);
    /* Fire expired high-resolution timers + reprogram LAPIC one-shot */
    extern void hrtimer_run_expired(void);
    hrtimer_run_expired();
}

uint64_t irq_get_ticks(void) { return tick_count; }

/* ── Reschedule IPI ────────────────────────────────── */

static void resched_ipi_handler(int vector) {
    (void)vector;
    percpu_self()->need_resched = 1;
}

/* Send resched IPI to all other cores. Used after a wake_up that may have
 * set a thread's state to RUNNABLE on another idle CPU's horizon — this
 * forces the idle CPU out of HLT so it picks up the runnable thread.
 *
 * ICR low layout: bit 14 (level=assert), bits 18-19 (shorthand=11
 * all-excluding-self), bits 0-7 vector. */
void smp_resched_others(void) {
    volatile uint32_t *icr_lo = (volatile uint32_t *)(LAPIC_BASE + 0x300);
    *icr_lo = 0x000C4000u | 0xFDu;  /* all-excluding-self, level=assert, vector 0xFD */
}

/* ── Init ──────────────────────────────────────────── */

__attribute__((cold))
void irq_init(void) {
    hal_cpu_cli();

    /* ISR stub addresses are identity-mapped (EFI relocations).
     * Add PHYS_OFFSET so they resolve via direct map in user PML4. */
    for (int i = 0; i < 256; i++)
        idt_set_entry(i, ensure_high(isr_stub_table[i]));
    for (int i = 0; i < 32; i++)
        irq_register(i, default_exception);

    idt_set_entry_user(0x80, ensure_high(isr_stub_table[0x80]));
    /* int3 (breakpoint) must be DPL=3 for userspace abort()/SIGTRAP */
    idt_set_entry_user(3, ensure_high(isr_stub_table[3]));

    /* TLB shootdown IPI vector */
    irq_register(0xFE, tlb_shootdown_handler);

    /* Reschedule IPI (0xFD): sets need_resched flag on target core.
     * Breaks hlt + triggers reschedule on next sched_loop iteration. */
    irq_register(0xFD, resched_ipi_handler);

    idtp.limit = sizeof(idt) - 1;
    idtp.base = ensure_high((uint64_t)(uintptr_t)&idt);
    hal_irq_install_vector_table(&idtp);
    serial_puts("IRQ: IDT loaded\n");

    for (int i = 0; i < 24; i++) {
        uint32_t lo = ioapic_read(0x10 + i * 2);
        ioapic_write(0x10 + i * 2, lo | (1 << 16));
    }

    lapic_write(LAPIC_SVR, 0x1FF);
    lapic_write(LAPIC_TPR, 0);
    serial_puts("IRQ: LAPIC enabled\n");

    irq_register(32, timer_handler);
    lapic_write(LAPIC_TIMER_DIV, 0x03);
    lapic_write(LAPIC_TIMER, 0x20020);    /* periodic, vector 32 */
    lapic_write(LAPIC_TIMER_INIT, 100000); /* 1000Hz (1ms) per-CPU LAPIC tick */
    serial_puts("IRQ: LAPIC Timer 1000Hz\n");

    hal_cpu_sti();
}
