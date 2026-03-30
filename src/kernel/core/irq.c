/* CosmoRT Interrupt handling — APIC + IDT, thread-aware */

#include "core/irq.h"
#include "core/timer.h"
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
#include "memops.h"

#define VECTOR_DE        0
#define VECTOR_BP        3
#define VECTOR_UD        6
#define VECTOR_GP        13
#define VECTOR_PF        14
#define VECTOR_MF        16
#define VECTOR_XF        19
#define VECTOR_TIMER     32
#define VECTOR_SYSCALL   0x80
#define VECTOR_RESCHED   0xFD
#define VECTOR_TLB_SHOOT 0xFE

#define LAPIC_PHYS       0xFEE00000
#define LAPIC_BASE       (LAPIC_PHYS + PHYS_OFFSET)
#define LAPIC_EOI        0x0B0
#define LAPIC_SVR        0x0F0
#define LAPIC_TPR        0x080
#define LAPIC_TIMER      0x320
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0

#define LAPIC_LVT_PERIODIC (1 << 17)
#define LAPIC_LVT_TIMER_PERIODIC (LAPIC_LVT_PERIODIC | VECTOR_TIMER)
#define LAPIC_LVT_TIMER_ONESHOT  (VECTOR_TIMER)

static uint32_t lapic_ticks_per_ms = 100000;

static volatile uint32_t *lapic = (volatile uint32_t *)LAPIC_BASE;

static void lapic_write(uint32_t reg, uint32_t val) { lapic[reg/4] = val; }

void lapic_eoi(void) { lapic_write(LAPIC_EOI, 0); }

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

void ioapic_route_irq_level(uint8_t irq, uint8_t vector) {
    ioapic_write(0x10 + irq*2, (uint32_t)vector | (1 << 13) | (1 << 15));
    ioapic_write(0x10 + irq*2 + 1, 0);
}

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

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
} irq_frame_t;

static void default_exception_with_frame(int vector, irq_frame_t *frame);

__attribute__((cold, noreturn))
static void pf_kernel_panic(uint64_t cr2, uint64_t error, uint64_t rip) {
    serial_puts("\nPAGE FAULT in kernel CR2=");
    serial_hex64(cr2);
    serial_puts(" err=");
    serial_hex64(error);
    serial_puts(" rip=");
    serial_hex64(rip);
    serial_puts(" KERNEL PANIC\n");
    arch_cli_halt();
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

#define MAX_HANDLERS 256
static irq_handler_t handlers[MAX_HANDLERS];
volatile int irq_event_pending = 0;

void irq_register(int vector, irq_handler_t handler) {
    if (vector >= 0 && vector < MAX_HANDLERS)
        handlers[vector] = handler;
}

__attribute__((hot))
void irq_dispatch(int vector, irq_frame_t *frame) {
    if (vector == VECTOR_SYSCALL) {
        long sysnum = (long)frame->rax;
        frame->rax = (uint64_t)sys_handler(
            sysnum, (long)frame->rdi, (long)frame->rsi,
            (long)frame->rdx, (long)frame->r10, (long)frame->r8,
            (long)frame->r9);
        if (sysnum != SYS_RT_SIGRETURN) {
            extern void check_pending_signals(void);
            thread_t *t = percpu_self()->current_thread;
            if (t && t->proc && (t->proc->sig_pending & ~t->sig_blocked)) {
                t->r15 = frame->r15; t->r14 = frame->r14;
                t->r13 = frame->r13; t->r12 = frame->r12;
                t->r11 = frame->r11; t->r10 = frame->r10;
                t->r9  = frame->r9;  t->r8  = frame->r8;
                t->rbp = frame->rbp; t->rdi = frame->rdi;
                t->rsi = frame->rsi; t->rdx = frame->rdx;
                t->rcx = frame->rcx; t->rbx = frame->rbx;
                t->rax = frame->rax;
                t->rip = frame->rip; t->rflags = frame->rflags;
                t->rsp = frame->rsp;
                check_pending_signals();
                frame->r15 = t->r15; frame->r14 = t->r14;
                frame->r13 = t->r13; frame->r12 = t->r12;
                frame->r11 = t->r11; frame->r10 = t->r10;
                frame->r9  = t->r9;  frame->r8  = t->r8;
                frame->rbp = t->rbp; frame->rdi = t->rdi;
                frame->rsi = t->rsi; frame->rdx = t->rdx;
                frame->rcx = t->rcx; frame->rbx = t->rbx;
                frame->rax = t->rax;
                frame->rip = t->rip; frame->rflags = t->rflags;
                frame->rsp = t->rsp;
            }
        }
        return;
    }

    if (vector == 14) {
        uint64_t cr2 = arch_get_cr2();
        uint64_t error = frame->error;

        if (!(error & 4)) {
            if (cr2 < 0x800000000000ULL) {
                percpu_t *kcpu = percpu_self();
                thread_t *kt = kcpu->current_thread;
                process_t *kp = kt ? kt->proc : 0;
                if (kp) {
                    uint64_t kflags;
                    if (spin_trylock_irq(&kp->lock, &kflags)) {
                        vma_t *kvma = vma_find(kp->vma_root, cr2);
                        int kprot = kvma ? kvma->prot : 0;
                        int knp = kvma && !(error & 1);
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
                                uint64_t foff = k_file_offset + (kpage_addr - k_vma_start);
                                uint64_t phys = 0;
                                if (k_shared)
                                    phys = page_cache_lookup(k_file_ino, foff);
                                int kmap_prot = kprot;
                                if (k_shared && (kprot & PROT_WRITE))
                                    kmap_prot = kprot & ~PROT_WRITE;
                                if (phys) {
                                    page_incref(phys);
                                    if (map_user_page(kp->pml4, kpage_addr, phys, kmap_prot) == 0)
                                        return;
                                    page_free(phys_to_virt(phys));
                                } else {
                                    uint64_t *kpg = alloc_page();
                                    if (kpg) {
                                        extern long vfs_pread_by_ino(int, uint64_t, void *, size_t, size_t);
                                        vfs_pread_by_ino(k_file_backend, k_file_ino, kpg, (size_t)foff, 4096);
                                        phys = virt_to_phys(kpg);
                                        if (k_shared)
                                            page_cache_insert(k_file_ino, foff, phys);
                                        if (map_user_page(kp->pml4, kpage_addr, phys, kmap_prot) == 0)
                                            return;
                                        page_free(kpg);
                                    }
                                }
                            } else {
                                uint64_t *kpage = alloc_page();
                                if (kpage) {
                                    if (map_user_page(kp->pml4, kpage_addr,
                                                      virt_to_phys(kpage), kprot) == 0) {
                                        return;
                                    }
                                    page_free(kpage);
                                }
                            }
                        }
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
                                        arch_invlpg(kpage_addr);
                                        return;
                                    }
                                } else {
                                    uint64_t *knew = alloc_page();
                                    if (knew) {
                                        kmemcpy(knew, phys_to_virt(old_phys), 4096);
                                        if (map_user_page(kp->pml4, kpage_addr,
                                                          virt_to_phys(knew), kprot) == 0) {
                                            arch_invlpg(kpage_addr);
                                            page_free(phys_to_virt(old_phys));
                                            return;
                                        }
                                        page_free(knew);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            {
                percpu_t *kfcpu = percpu_self();
                if (kfcpu->fault_recover && cr2 < 0x800000000000ULL) {
                    kfcpu->fault_recover = 0;
                    uint64_t *jb = kfcpu->fault_jmpbuf;
                    frame->rbx = jb[0];
                    frame->rbp = jb[1];
                    frame->r12 = jb[2];
                    frame->r13 = jb[3];
                    frame->r14 = jb[4];
                    frame->r15 = jb[5];
                    frame->rsp = jb[6] + 8;
                    frame->rip = jb[7];
                    frame->rax = 1;
                    return;
                }
            }
            pf_kernel_panic(cr2, error, frame->rip);
        }

        percpu_t *cpu = percpu_self();
        thread_t *t = cpu->current_thread;
        process_t *p = t ? t->proc : 0;

        if (p) {
            uint64_t vma_flags;
            spin_lock_irq(&p->lock, &vma_flags);
            vma_t *vma = vma_find(p->vma_root, cr2);
            if (vma) {
                if ((error & 1) && (error & 2) && !(vma->prot & PROT_WRITE)) {
                    spin_unlock_irq(&p->lock, vma_flags);
                    goto kill_process;
                }
                if ((error & 1) && (error & 2) && (vma->prot & PROT_WRITE)) {
                    int cow_prot = vma->prot;
                    spin_unlock_irq(&p->lock, vma_flags);
                    uint64_t page_addr = cr2 & ~0xFFFULL;
                    extern uint64_t read_pte_pub(uint64_t *pml4, uint64_t va);
                    uint64_t pte = read_pte_pub(p->pml4, page_addr);
                    #define PTE_COW_IRQ (1ULL << 9)
                    if ((pte & 1) && !(pte & PTE_COW_IRQ)) {
                        uint64_t phys = pte & 0x000FFFFFFFFFF000ULL;
                        if (map_user_page(p->pml4, page_addr, phys, cow_prot) == 0) {
                            arch_invlpg(page_addr);
                            return;
                        }
                        goto kill_process;
                    }
                    if ((pte & PTE_COW_IRQ) && (pte & 1)) {
                        uint64_t old_phys = pte & 0x000FFFFFFFFFF000ULL;
                        int rc = page_refcount(old_phys);
                        if (rc <= 1) {
                            if (map_user_page(p->pml4, page_addr, old_phys, cow_prot) == 0) {
                                arch_invlpg(page_addr);
                                return;
                            }
                        } else {
                            uint64_t *new_page = alloc_page();
                            if (new_page) {
                                kmemcpy(new_page, phys_to_virt(old_phys), 4096);
                                if (map_user_page(p->pml4, page_addr,
                                                  virt_to_phys(new_page), cow_prot) == 0) {
                                    arch_invlpg(page_addr);
                                    page_free(phys_to_virt(old_phys));
                                    return;
                                }
                                page_free(new_page);
                            }
                        }
                    }
                    goto kill_process;
                }
                if ((error & 1) && (error & 0x10) && (vma->prot & PROT_EXEC)) {
                    int nx_prot = vma->prot;
                    spin_unlock_irq(&p->lock, vma_flags);
                    uint64_t page_addr = cr2 & ~0xFFFULL;
                    extern uint64_t read_pte_pub(uint64_t *pml4, uint64_t va);
                    uint64_t pte = read_pte_pub(p->pml4, page_addr);
                    if (pte & 1) {
                        uint64_t phys = pte & 0x000FFFFFFFFFF000ULL;
                        if (map_user_page(p->pml4, page_addr, phys, nx_prot) == 0) {
                            arch_invlpg(page_addr);
                            return;
                        }
                    }
                    goto kill_process;
                }
                if (!(error & 1) && (vma->prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) {
                    int dp_prot = vma->prot;
                    int dp_huge = (vma->flags & VMA_HUGEPAGE);
                    uint64_t vma_start = vma->start;
                    uint64_t vma_end = vma->end;
                    uint64_t dp_file_ino = vma->file_ino;
                    uint64_t dp_file_offset = vma->file_offset;
                    int dp_file_backend = vma->file_backend;
                    int dp_shared = (vma->flags & VMA_SHARED);
                    spin_unlock_irq(&p->lock, vma_flags);
                    uint64_t page_addr = cr2 & ~0xFFFULL;

                    if (__builtin_expect(dp_file_ino != 0, 0)) {
                        uint64_t foff = dp_file_offset + (page_addr - vma_start);
                        uint64_t phys = 0;
                        if (dp_shared)
                            phys = page_cache_lookup(dp_file_ino, foff);
                        int map_prot = dp_prot;
                        if (dp_shared && (dp_prot & PROT_WRITE))
                            map_prot = dp_prot & ~PROT_WRITE;
                        if (phys) {
                            page_incref(phys);
                            if (map_user_page(p->pml4, page_addr, phys, map_prot) == 0)
                                return;
                            page_free(phys_to_virt(phys));
                        } else {
                            uint64_t *pg = alloc_page();
                            if (pg) {
                                extern long vfs_pread_by_ino(int, uint64_t, void *, size_t, size_t);
                                vfs_pread_by_ino(dp_file_backend, dp_file_ino, pg, (size_t)foff, 4096);
                                phys = virt_to_phys(pg);
                                if (dp_shared)
                                    page_cache_insert(dp_file_ino, foff, phys);
                                if (map_user_page(p->pml4, page_addr, phys, map_prot) == 0)
                                    return;
                                page_free(pg);
                            }
                        }
                        goto kill_process;
                    }

                    if (dp_huge) {
                        uint64_t huge_base = cr2 & ~0x1FFFFFULL;
                        if (huge_base >= vma_start &&
                            huge_base + 0x200000ULL <= vma_end) {
                            void *hp = huge_page_alloc();
                            if (hp) {
                                if (map_user_huge_page(p->pml4, huge_base,
                                                       virt_to_phys(hp), dp_prot) == 0) {
                                    return;
                                }
                                huge_page_free(hp);
                            }
                        }
                    }

                    uint64_t *page = alloc_page();
                    if (page) {
                        if (map_user_page(p->pml4, page_addr,
                                          virt_to_phys(page), dp_prot) == 0) {
                            return;
                        }
                        page_free(page);
                    }
                    goto kill_process;
                }
            }
            spin_unlock_irq(&p->lock, vma_flags);
        }

    kill_process:
        if (t && t->proc) {
            process_t *faultp = t->proc;
            struct k_sigaction *sa = &faultp->sig_actions[SIGSEGV];
            if ((uint64_t)sa->sa_handler > 1 && !(t->sig_blocked & SIG_BIT(SIGSEGV))) {
                t->fault_addr = cr2;
                t->rip = frame->rip;
                t->rsp = frame->rsp;
                t->rflags = frame->rflags;
                t->rax = frame->rax; t->rbx = frame->rbx;
                t->rcx = frame->rcx; t->rdx = frame->rdx;
                t->rsi = frame->rsi; t->rdi = frame->rdi;
                t->rbp = frame->rbp;
                t->r8  = frame->r8;  t->r9  = frame->r9;
                t->r10 = frame->r10; t->r11 = frame->r11;
                t->r12 = frame->r12; t->r13 = frame->r13;
                t->r14 = frame->r14; t->r15 = frame->r15;

                extern void deliver_signal(thread_t *t, int signo);
                deliver_signal(t, SIGSEGV);

                frame->rip = t->rip;
                frame->rsp = t->rsp;
                frame->rflags = t->rflags;
                frame->rax = t->rax; frame->rbx = t->rbx;
                frame->rcx = t->rcx; frame->rdx = t->rdx;
                frame->rsi = t->rsi; frame->rdi = t->rdi;
                frame->rbp = t->rbp;
                frame->r8  = t->r8;  frame->r9  = t->r9;
                frame->r10 = t->r10; frame->r11 = t->r11;
                frame->r12 = t->r12; frame->r13 = t->r13;
                frame->r14 = t->r14; frame->r15 = t->r15;
                return;
            }
        }

        segfault_log(cr2, frame->rip, error, (t && t->proc) ? (int)t->proc->pid : -1);
        if (t && t->proc) {
            extern void do_exit_group(int status);
            arch_set_cr3(virt_to_phys(t->proc->pml4));
            do_exit_group(139);
        }
        pf_kernel_panic(cr2, error, frame->rip);
    }

    if (vector < VECTOR_TIMER && vector != VECTOR_PF) {
        default_exception_with_frame(vector, frame);
        return;
    }

    if (vector == VECTOR_TIMER || vector == VECTOR_RESCHED) {
        extern void sched_preempt(void *frame);
        sched_preempt(frame);
    }

    if (vector < MAX_HANDLERS && handlers[vector])
        handlers[vector](vector);
    irq_event_pending = 1;
    lapic_eoi();
}

__attribute__((cold))
static void default_exception_with_frame(int vector, irq_frame_t *frame) {
    percpu_t *cpu = percpu_self();
    thread_t *t = cpu->current_thread;

    if (!(frame->cs & 3) && cpu->fault_recover) {
        cpu->fault_recover = 0;
        uint64_t *jb = cpu->fault_jmpbuf;
        frame->rbx = jb[0]; frame->rbp = jb[1];
        frame->r12 = jb[2]; frame->r13 = jb[3];
        frame->r14 = jb[4]; frame->r15 = jb[5];
        frame->rsp = jb[6] + 8;
        frame->rip = jb[7];
        frame->rax = 1;
        return;
    }

    if (t && t->proc && (frame->cs & 3)) {
        int signo = 0;
        if (vector == VECTOR_GP || vector == VECTOR_PF) signo = SIGSEGV;
        else if (vector == VECTOR_BP) signo = SIGTRAP;
        else if (vector == VECTOR_DE || vector == VECTOR_MF || vector == VECTOR_XF) signo = SIGFPE;
        else if (vector == VECTOR_UD) signo = SIGILL;

        if (signo) {
            struct k_sigaction *sa = &t->proc->sig_actions[signo];
            if ((uint64_t)sa->sa_handler > 1 && !(t->sig_blocked & SIG_BIT(signo))) {
                if (vector == VECTOR_PF) {
                    t->fault_addr = arch_get_cr2();
                } else {
                    t->fault_addr = frame->rip;
                }
                t->rip = frame->rip; t->rsp = frame->rsp;
                t->rflags = frame->rflags;
                t->rax = frame->rax; t->rbx = frame->rbx;
                t->rcx = frame->rcx; t->rdx = frame->rdx;
                t->rsi = frame->rsi; t->rdi = frame->rdi;
                t->rbp = frame->rbp;
                t->r8  = frame->r8;  t->r9  = frame->r9;
                t->r10 = frame->r10; t->r11 = frame->r11;
                t->r12 = frame->r12; t->r13 = frame->r13;
                t->r14 = frame->r14; t->r15 = frame->r15;

                extern void deliver_signal(thread_t *t, int signo);
                deliver_signal(t, signo);

                frame->rip = t->rip; frame->rsp = t->rsp;
                frame->rflags = t->rflags;
                frame->rax = t->rax; frame->rbx = t->rbx;
                frame->rcx = t->rcx; frame->rdx = t->rdx;
                frame->rsi = t->rsi; frame->rdi = t->rdi;
                frame->rbp = t->rbp;
                frame->r8  = t->r8;  frame->r9  = t->r9;
                frame->r10 = t->r10; frame->r11 = t->r11;
                frame->r12 = t->r12; frame->r13 = t->r13;
                frame->r14 = t->r14; frame->r15 = t->r15;
                return;
            }
        }
    }

    serial_puts("\nEXCEPTION ");
    serial_putchar('0' + vector / 10);
    serial_putchar('0' + vector % 10);
    serial_puts(" rip=");
    serial_hex64(frame->rip);
    serial_puts(" err=");
    serial_hex64(frame->error);
    serial_puts(" rsp=");
    serial_hex64(frame->rsp);

    if (vector == VECTOR_PF) {
        uint64_t cr2 = arch_get_cr2();
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
        serial_puts(" fs=");  serial_hex64(arch_get_fs_base());
        serial_puts(" killed\n");
        extern void do_exit_group(int status);
        arch_set_cr3(virt_to_phys(t->proc->pml4));
        t->proc->exit_signal = vector;
        do_exit_group(128 + vector);
    }

    serial_puts(" KERNEL PANIC\n");
    arch_cli_halt();
}

__attribute__((cold))
static void default_exception(int vector) {
    serial_puts("\nEXCEPTION ");
    serial_putchar('0' + vector / 10);
    serial_putchar('0' + vector % 10);
    serial_puts(" KERNEL PANIC\n");
    arch_cli_halt();
}

static volatile uint64_t shootdown_pml4 = 0;
static volatile int shootdown_ack = 0;
static spinlock_t shootdown_lock = SPINLOCK_INIT;

static void tlb_shootdown_handler(int vector) {
    (void)vector;
    uint64_t target = shootdown_pml4;
    if (target == 0) {
        arch_flush_tlb();
    } else {
        percpu_t *cpu = percpu_self();
        thread_t *t = cpu->current_thread;
        if (t && t->proc && virt_to_phys(t->proc->pml4) == target)
            arch_flush_tlb();
    }
    __sync_fetch_and_add(&shootdown_ack, 1);
}

void tlb_shootdown(uint64_t pml4_phys) {
    int ncores = smp_num_cores();
    if (ncores < 2) return;

    uint64_t flags;
    spin_lock_irq(&shootdown_lock, &flags);

    shootdown_ack = 0;
    shootdown_pml4 = pml4_phys;
    arch_mfence();

    volatile uint32_t *icr_lo = (volatile uint32_t *)(LAPIC_BASE + 0x300);
    *icr_lo = 0x000C0000 | VECTOR_TLB_SHOOT;

    int expected = ncores - 1;
    uint64_t sd_deadline = timer_ms() + 10;
    while (__sync_val_compare_and_swap(&shootdown_ack, expected, expected) < expected) {
        if (timer_ms() >= sd_deadline)
            break;
        arch_pause();
    }

    spin_unlock_irq(&shootdown_lock, flags);
}

static volatile uint64_t tick_count = 0;

static void timer_handler(int vector) {
    (void)vector;
    __sync_fetch_and_add(&tick_count, 1);
    extern void random_add_interrupt_entropy(void);
    random_add_interrupt_entropy();
    extern void serial_bridge_poll(void);
    serial_bridge_poll();
    extern int timer_poll(int max_work);
    timer_poll(0);
}

uint64_t irq_get_ticks(void) { return tick_count; }

static void resched_ipi_handler(int vector) {
    (void)vector;
    percpu_self()->need_resched = 1;
}

__attribute__((cold))
void irq_init(void) {
    arch_cli();

    for (int i = 0; i < 256; i++)
        idt_set_entry(i, ensure_high(isr_stub_table[i]));
    for (int i = 0; i < VECTOR_TIMER; i++)
        irq_register(i, default_exception);

    idt_set_entry_user(VECTOR_SYSCALL, ensure_high(isr_stub_table[VECTOR_SYSCALL]));
    idt_set_entry_user(VECTOR_BP, ensure_high(isr_stub_table[VECTOR_BP]));

    irq_register(VECTOR_TLB_SHOOT, tlb_shootdown_handler);

    irq_register(VECTOR_RESCHED, resched_ipi_handler);

    idtp.limit = sizeof(idt) - 1;
    idtp.base = ensure_high((uint64_t)(uintptr_t)&idt);
    arch_lidt(&idtp);
    serial_puts("IRQ: IDT loaded\n");

    for (int i = 0; i < 24; i++) {
        uint32_t lo = ioapic_read(0x10 + i * 2);
        ioapic_write(0x10 + i * 2, lo | (1 << 16));
    }

    lapic_write(LAPIC_SVR, 0x1FF);
    lapic_write(LAPIC_TPR, 0);
    serial_puts("IRQ: LAPIC enabled\n");

    irq_register(VECTOR_TIMER, timer_handler);
    lapic_write(LAPIC_TIMER_DIV, 0x03);
    lapic_write(LAPIC_TIMER, LAPIC_LVT_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_INIT, lapic_ticks_per_ms);
    serial_puts("IRQ: Timer 1000Hz (RT-Core)\n");

    arch_sti();
}

void lapic_delay_ms(uint32_t ms) {
    if (ms == 0) return;

    uint32_t count = ms * lapic_ticks_per_ms;
    lapic_write(LAPIC_TIMER, LAPIC_LVT_TIMER_ONESHOT);
    lapic_write(LAPIC_TIMER_INIT, count);

    while (lapic[LAPIC_TIMER_CUR / 4] > 0)
        arch_halt();

    lapic_write(LAPIC_TIMER, LAPIC_LVT_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_INIT, lapic_ticks_per_ms);
}
