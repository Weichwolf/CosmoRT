/* CosmoRT Interrupt handling — APIC + IDT, thread-aware */

#include "irq.h"
#include "serial.h"
#include "thread.h"
#include "process.h"
#include "percpu.h"
#include "syscall.h"
#include "config.h"
#include "vma.h"
#include "page_alloc.h"
#include "smp.h"
#include "spinlock.h"

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

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
} irq_frame_t;

/* Forward declaration */
static void default_exception_with_frame(int vector, irq_frame_t *frame);

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
                /* Save irq frame → thread_t */
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
                /* Write back thread_t → irq frame */
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

    /* Page fault (vector 14): demand paging */
    if (vector == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
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
                        int knp = kvma && !(error & 1);
                        spin_unlock_irq(&kp->lock, kflags);
                        if (knp) {
                            uint64_t kpage_addr = cr2 & ~0xFFFULL;
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
                    /* trylock failed → fall through to fault_recover */
                }
            }
            /* Kernel accessed unmapped user address (e.g. bad pointer from syscall).
             * If fault_recover is armed, resume execution at the setjmp return
             * point by restoring callee-saved registers and RSP/RIP from the
             * jmpbuf into the IRQ frame. The setjmp will return 1 (via RAX). */
            {
                percpu_t *kfcpu = percpu_self();
                if (kfcpu->fault_recover && cr2 < 0x800000000000ULL) {
                    kfcpu->fault_recover = 0;
                    /* jmpbuf layout: [rbx, rbp, r12, r13, r14, r15, rsp, rip] */
                    uint64_t *jb = kfcpu->fault_jmpbuf;
                    frame->rbx = jb[0];
                    frame->rbp = jb[1];
                    frame->r12 = jb[2];
                    frame->r13 = jb[3];
                    frame->r14 = jb[4];
                    frame->r15 = jb[5];
                    frame->rsp = jb[6] + 8; /* +8: skip return addr (not popped by ret) */
                    frame->rip = jb[7];
                    frame->rax = 1; /* setjmp return value */
                    return; /* IRET will resume at setjmp return */
                }
            }
            serial_puts("\nPAGE FAULT in kernel CR2=");
            serial_hex64(cr2);
            serial_puts(" err=");
            serial_hex64(error);
            serial_puts(" rip=");
            serial_hex64(frame->rip);
            serial_puts(" KERNEL PANIC\n");
            __asm__ volatile("cli; hlt");
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
                /* Protection violation: check write permission */
                if ((error & 1) && (error & 2) && !(vma->prot & PROT_WRITE)) {
                    /* Write to read-only VMA → kill */
                    spin_unlock_irq(&p->lock, vma_flags);
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
                            __asm__ volatile("invlpg (%0)" :: "r"(page_addr) : "memory");
                            return; /* resume execution */
                        }
                    }
                    goto kill_process;
                }
                /* Not-present fault in a valid VMA → allocate page.
                 * PROT_NONE VMAs must NOT be demand-paged — access = SIGSEGV. */
                if (!(error & 1) && (vma->prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) {
                    int dp_prot = vma->prot;
                    spin_unlock_irq(&p->lock, vma_flags);
                    uint64_t page_addr = cr2 & ~0xFFFULL;
                    uint64_t *page = alloc_page();
                    if (page) {
                        if (map_user_page(p->pml4, page_addr,
                                          virt_to_phys(page), dp_prot) == 0) {
                            return; /* resume execution */
                        }
                        page_free(page);
                    }
                    goto kill_process;
                }
            }
            spin_unlock_irq(&p->lock, vma_flags);
        }

    kill_process:
        /* Try to deliver SIGSEGV to user handler before killing */
        if (t && t->proc) {
            process_t *faultp = t->proc;
            struct k_sigaction *sa = &faultp->sig_actions[11]; /* SIGSEGV=11 */
            if ((uint64_t)sa->sa_handler > 1 && !(t->sig_blocked & (1ULL << 11))) {
                /* User SIGSEGV handler registered and not blocked — deliver signal.
                 * Save IRQ frame into thread_t, deliver, write back. */
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
                deliver_signal(t, 11);

                /* Write back modified registers to IRQ frame */
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
                return; /* resume into signal handler */
            }
        }

        serial_puts("\nSEGFAULT CR2=");
        serial_hex64(cr2);
        serial_puts(" RIP=");
        serial_hex64(frame->rip);
        serial_puts(" err=");
        serial_hex64(error);
        if (t && t->proc) {
            serial_puts(" pid=");
            serial_putchar('0' + (t->proc->pid % 10));
            serial_puts(" killed\n");
            /* Use do_exit_group to properly close FDs, wake parent, etc. */
            extern void do_exit_group(int status);
            /* Switch to user page tables for exit (FD cleanup may access user ptrs) */
            __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(t->proc->pml4)) : "memory");
            do_exit_group(139); /* SIGSEGV */
            /* do_exit_group calls thread_return_to_kernel internally */
        }
        serial_puts(" KERNEL PANIC\n");
        __asm__ volatile("cli; hlt");
    }

    /* Other CPU exceptions (0-31, except 14 handled above): use frame-aware handler */
    if (vector < 32 && vector != 14) {
        default_exception_with_frame(vector, frame);
        return;
    }

    /* Timer (vector 32): RT scheduler preemption */
    if (vector == 32) {
        /* (tick debug removed) */
        extern void sched_preempt(void *frame);
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
            if ((uint64_t)sa->sa_handler > 1 && !(t->sig_blocked & (1ULL << signo))) {
                /* Deliver signal via IRQ frame → thread_t → deliver_signal → IRQ frame */
                if (vector == 14) {
                    __asm__ volatile("mov %%cr2, %0" : "=r"(t->fault_addr));
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
                return; /* resume into signal handler */
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

    if (vector == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        serial_puts(" CR2="); serial_hex64(cr2);
    }

    if (t && t->proc) {
        serial_puts(" pid=");
        serial_putchar('0' + (t->proc->pid % 10));
        serial_puts(" tid=");
        serial_putchar('0' + (t->tid % 10));
        serial_puts(" killed\n");
        /* Use do_exit_group to properly close FDs, wake parent, etc. */
        extern void do_exit_group(int status);
        __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(t->proc->pml4)) : "memory");
        t->proc->exit_signal = vector;
        do_exit_group(128 + vector);
    }

    serial_puts(" KERNEL PANIC\n");
    __asm__ volatile("cli; hlt");
}

__attribute__((cold))
static void default_exception(int vector) {
    /* Legacy path without frame — shouldn't be called for exceptions
     * that go through irq_dispatch with frame, but kept as fallback */
    serial_puts("\nEXCEPTION ");
    serial_putchar('0' + vector / 10);
    serial_putchar('0' + vector % 10);
    serial_puts(" KERNEL PANIC\n");
    __asm__ volatile("cli; hlt");
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
        __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    } else {
        percpu_t *cpu = percpu_self();
        thread_t *t = cpu->current_thread;
        if (t && t->proc && virt_to_phys(t->proc->pml4) == target)
            __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
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
    __asm__ volatile("mfence" ::: "memory");

    /* Send IPI to all other cores: all-excluding-self shorthand, vector 0xFE */
    volatile uint32_t *icr_lo = (volatile uint32_t *)(LAPIC_BASE + 0x300);
    *icr_lo = 0x000C0000 | 0xFE;

    /* Wait for all other cores to ACK (with timeout to avoid deadlock) */
    int expected = ncores - 1;
    for (int i = 0; i < 10000000; i++) {
        if (__sync_val_compare_and_swap(&shootdown_ack, expected, expected) >= expected)
            break;
        __asm__ volatile("pause");
    }

    spin_unlock_irq(&shootdown_lock, flags);
}

/* ── Timer ─────────────────────────────────────────── */

static volatile uint64_t tick_count = 0;

static void timer_handler(int vector) {
    (void)vector;
    __sync_fetch_and_add(&tick_count, 1);
    extern void random_add_interrupt_entropy(void);
    random_add_interrupt_entropy();
    /* Poll serial RX → PTY input (serial console bridge) */
    extern void serial_bridge_poll(void);
    serial_bridge_poll();
    /* NIC polling moved to E1000 IRQ handler — not safe in timer context
     * due to reentrancy with spinlocks in net_poll/epoll_wake_all. */
}

uint64_t irq_get_ticks(void) { return tick_count; }

/* ── Init ──────────────────────────────────────────── */

__attribute__((cold))
void irq_init(void) {
    __asm__ volatile("cli");

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

    idtp.limit = sizeof(idt) - 1;
    idtp.base = ensure_high((uint64_t)(uintptr_t)&idt);
    __asm__ volatile("lidt %0" : : "m"(idtp));
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
    lapic_write(LAPIC_TIMER, 0x20020);
    lapic_write(LAPIC_TIMER_INIT, 10000000);
    serial_puts("IRQ: Timer started\n");

    __asm__ volatile("sti");
}
