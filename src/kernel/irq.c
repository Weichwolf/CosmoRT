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
            if (t && t->proc && (t->proc->sig_pending & ~t->proc->sig_blocked)) {
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

        /* Kernel-mode fault (bit 2 = 0) → panic */
        if (!(error & 4)) {
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
            vma_t *vma = vma_find(p->vma_root, cr2);
            if (vma) {
                /* Protection violation: check write permission */
                if ((error & 1) && (error & 2) && !(vma->prot & PROT_WRITE)) {
                    /* Write to read-only VMA → kill */
                    goto kill_process;
                }
                /* Not-present fault in a valid VMA → allocate page */
                if (!(error & 1)) {
                    uint64_t page_addr = cr2 & ~0xFFFULL;
                    uint64_t *page = alloc_page();
                    if (page) {
                        if (map_user_page(p->pml4, page_addr,
                                          virt_to_phys(page), vma->prot) == 0) {
                            return; /* resume execution */
                        }
                        page_free(page);
                    }
                }
            }
        }

    kill_process:
        serial_puts("\nSEGFAULT CR2=");
        serial_hex64(cr2);
        if (t && t->proc) {
            serial_puts(" pid=");
            serial_putchar('0' + (t->proc->pid % 10));
            serial_puts(" killed\n");
            t->state = THREAD_DEAD;
            t->proc->state = PROC_ZOMBIE;
            extern uint64_t pml4[];
            __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(pml4)) : "memory");
            thread_return_to_kernel(t);
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
        extern void sched_preempt(void *frame);
        sched_preempt(frame);
    }

    if (vector < MAX_HANDLERS && handlers[vector])
        handlers[vector](vector);
    irq_event_pending = 1;
    lapic_eoi();
}

/* Exception handler — kills user thread, halts on kernel fault */

static void default_exception_with_frame(int vector, irq_frame_t *frame) {
    percpu_t *cpu = percpu_self();
    thread_t *t = cpu->current_thread;

    serial_puts("\nEXCEPTION ");
    serial_putchar('0' + vector / 10);
    serial_putchar('0' + vector % 10);
    serial_puts(" rip=");
    serial_hex64(frame->rip);
    serial_puts(" err=");
    serial_hex64(frame->error);

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

        t->state = THREAD_DEAD;
        t->proc->state = PROC_ZOMBIE;
        extern uint64_t pml4[];
        __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(pml4)) : "memory");
        thread_return_to_kernel(t);
    }

    serial_puts(" KERNEL PANIC\n");
    __asm__ volatile("cli; hlt");
}

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

static void tlb_shootdown_handler(int vector) {
    (void)vector;
    percpu_t *cpu = percpu_self();
    thread_t *t = cpu->current_thread;
    if (t && t->proc && virt_to_phys(t->proc->pml4) == shootdown_pml4) {
        __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    }
    lapic_eoi();
}

void tlb_shootdown(uint64_t pml4_phys) {
    shootdown_pml4 = pml4_phys;
    __asm__ volatile("mfence" ::: "memory");
    /* Send IPI to all other cores: all-excluding-self shorthand, vector 0xFE */
    volatile uint32_t *icr_lo = (volatile uint32_t *)(LAPIC_BASE + 0x300);
    *icr_lo = 0x000C0000 | 0xFE;  /* dest shorthand=all-excl-self, vector=0xFE */
    for (volatile int i = 0; i < 1000; i++) __asm__ volatile("pause");
}

/* ── Timer ─────────────────────────────────────────── */

static volatile uint64_t tick_count = 0;

static void timer_handler(int vector) {
    (void)vector;
    __sync_fetch_and_add(&tick_count, 1);
    extern void random_add_interrupt_entropy(void);
    random_add_interrupt_entropy();
    /* No net_poll here — E1000 uses IRQ-driven receive */
}

uint64_t irq_get_ticks(void) { return tick_count; }

/* ── Init ──────────────────────────────────────────── */

void irq_init(void) {
    __asm__ volatile("cli");

    /* ISR stub addresses are identity-mapped (EFI relocations).
     * Add PHYS_OFFSET so they resolve via direct map in user PML4. */
    for (int i = 0; i < 256; i++)
        idt_set_entry(i, ensure_high(isr_stub_table[i]));
    for (int i = 0; i < 32; i++)
        irq_register(i, default_exception);

    idt_set_entry_user(0x80, ensure_high(isr_stub_table[0x80]));

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
