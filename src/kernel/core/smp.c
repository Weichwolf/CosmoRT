/* SMP — INIT/SIPI AP wakeup for x86_64
 *
 * Copies 16-bit trampoline to 0x8000, fills data area at 0x8F00,
 * sends INIT + SIPI broadcast. Each AP transitions 16→32→64 bit,
 * loads kernel PML4, picks its stack by APIC ID, jumps to ap_main.
 */

#include "config.h"
#include "core/smp.h"
#include "hw/serial.h"
#include "core/timer.h"
#include "core/percpu.h"
#include "memops.h"
#include "arch/arch.h"

/* Trampoline binary (assembled from ap_trampoline.asm → flat binary → C header) */
#include "gen/ap_trampoline_bin.h"

/* LAPIC MMIO (direct-map addresses) */
#define LAPIC_ID      (0xFEE00020ULL + PHYS_OFFSET)
#define LAPIC_ICR_LO  (0xFEE00300ULL + PHYS_OFFSET)
#define LAPIC_ICR_HI  (0xFEE00310ULL + PHYS_OFFSET)

/* Trampoline is placed at physical 0x8000. SIPI vector = 0x08. */
#define TRAMP_PHYS    0x8000ULL
#define TRAMP_VECTOR  (TRAMP_PHYS >> 12)  /* = 0x08 */
#define TRAMP_DATA    0x8F00ULL           /* data area within trampoline page */

/* ICR fields (Intel SDM Vol 3A §10.6.1) */
#define ICR_INIT_ASSERT    0x0000C500  /* INIT, level-triggered, assert */
#define ICR_INIT_DEASSERT  0x00008500  /* INIT, level-triggered, de-assert */
#define ICR_SIPI           0x00000600  /* Startup IPI (OR with vector) */
#define ICR_DELIVERY_PENDING (1 << 12)

extern uint64_t pml4[];

/* Per-core state */
static volatile int core_alive[SMP_MAX_CORES];
static int total_cores = 1;

/* Per-core stacks */
static uint8_t core_stacks[SMP_MAX_CORES][SMP_STACK_SIZE] __attribute__((aligned(16)));

static void (*user_entry_fn)(void);

/* AP C entry — each AP lands here after trampoline */
#define LAPIC_SVR        (0xFEE000F0ULL + PHYS_OFFSET)
#define LAPIC_TPR        (0xFEE00080ULL + PHYS_OFFSET)
#define LAPIC_TIMER_REG  (0xFEE00320ULL + PHYS_OFFSET)
#define LAPIC_TIMER_INIT (0xFEE00380ULL + PHYS_OFFSET)
#define LAPIC_TIMER_DIV  (0xFEE003E0ULL + PHYS_OFFSET)
#define LAPIC_EOI_REG    (0xFEE000B0ULL + PHYS_OFFSET)

static void ap_main(void) {
    /* Enable SSE: CR0.EM=0 (no x87 emulation), CR0.MP=1 (monitor coprocessor),
     * CR4.OSFXSR=1 (FXSAVE/FXRSTOR), CR4.OSXMMEXCPT=1 (SIMD exceptions).
     * Without CR0.EM clear, any SSE instruction faults (#NM) on this core. */
    uint64_t cr0 = arch_get_cr0();
    cr0 &= ~(1ULL << 2);  /* clear EM */
    cr0 &= ~(1ULL << 3);  /* clear TS (allow FPU/SSE without #NM) */
    cr0 |= (1ULL << 1);   /* set MP */
    arch_set_cr0(cr0);
    uint64_t cr4 = arch_get_cr4();
    cr4 |= (1 << 9) | (1 << 10);  /* OSFXSR + OSXMMEXCPT */

    /* SMEP + SMAP on AP (same CPUID check as BSP) */
    uint32_t eax, ebx, ecx, edx;
    arch_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
    if (ebx & (1U << 7))  cr4 |= (1ULL << 20);  /* SMEP */
    if (ebx & (1U << 20)) cr4 |= (1ULL << 21);  /* SMAP */

    arch_set_cr4(cr4);
    arch_clac();  /* ensure AC=0 — kernel default */

    /* Initialize this core's LAPIC (reset by INIT IPI) */
    *(volatile uint32_t *)LAPIC_SVR = 0x1FF;         /* enable, spurious=0xFF */
    *(volatile uint32_t *)LAPIC_TPR = 0;              /* accept all */
    *(volatile uint32_t *)LAPIC_TIMER_DIV = 0x03;     /* divide by 16 */
    *(volatile uint32_t *)LAPIC_TIMER_REG = 0x20020;  /* periodic, vector 32 */
    *(volatile uint32_t *)LAPIC_TIMER_INIT = 100000;   /* 1000Hz (1ms) — match BSP for responsive scheduling */

    volatile uint32_t *lapic_id_reg = (volatile uint32_t *)LAPIC_ID;
    uint32_t apic_id = (*lapic_id_reg >> 24) & 0xFF;

    if (apic_id < SMP_MAX_CORES)
        __sync_val_compare_and_swap(&core_alive[apic_id], 0, 1);

    /* Per-CPU data init for this AP */
    percpu_init_ap((int)apic_id);

    /* Per-core TSS (needed for Ring 3 → Ring 0 transitions) */
    extern void tss_init_ap(void);
    tss_init_ap();

    /* Set up SYSCALL/SYSRET + EFER.NXE on this core */
    extern void syscall_init(void);
    syscall_init();

    serial_puts("SMP: Core ");
    if (apic_id >= 10) serial_putchar('0' + (apic_id / 10));
    serial_putchar('0' + (apic_id % 10));
    serial_puts(" alive\n");

    if (user_entry_fn) user_entry_fn();

    for (;;) arch_halt();
}

/* (icr_wait removed — QEMU TCG leaves delivery-pending stuck) */

/* Write to LAPIC ICR (all-excluding-self broadcast) */
static void lapic_ipi_broadcast(uint32_t icr_lo_val) {
    volatile uint32_t *icr_lo = (volatile uint32_t *)LAPIC_ICR_LO;
    /* All-excluding-self shorthand = bits 19:18 = 11 = 0xC0000 */
    *icr_lo = 0x000C0000 | icr_lo_val;
}

static void serial_uint(uint64_t v) {
    char t[20]; int i = 0;
    do { t[i++] = '0' + (char)(v % 10); v /= 10; } while (v);
    while (i--) serial_putchar(t[i]);
}

int smp_start_all(void (*entry_fn)(void)) {
    user_entry_fn = entry_fn;
    core_alive[0] = 1;

    /* 1. Copy trampoline to physical 0x8000 (via direct map) */
    uint8_t *tramp_dst = (uint8_t *)phys_to_virt(TRAMP_PHYS);
    kmemcpy(tramp_dst, ap_trampoline_bin, ap_trampoline_bin_size);

    /* 2. Fill data area at 0x8F00 */
    volatile uint64_t *data = (volatile uint64_t *)phys_to_virt(TRAMP_DATA);
    data[0] = virt_to_phys(pml4);                          /* +0x00: CR3 */
    data[1] = (uint64_t)(uintptr_t)core_stacks;            /* +0x08: stack base (direct-map) */
    data[2] = ensure_high((uint64_t)(uintptr_t)ap_main);   /* +0x10: entry (direct-map) */
    /* +0x18: kernel GDT pointer (10 bytes: 2-byte limit + 8-byte base) */
    {
        arch_desc_t gdt_desc = arch_sgdt();
        uint16_t *gdt_limit = (uint16_t *)phys_to_virt(TRAMP_DATA + 0x18);
        uint64_t *gdt_base  = (uint64_t *)phys_to_virt(TRAMP_DATA + 0x1A);
        *gdt_limit = gdt_desc.limit;
        *gdt_base  = gdt_desc.base;
    }
    data[5] = SMP_STACK_SIZE;                               /* +0x28: stack size */
    /* +0x30: kernel IDT pointer (10 bytes: 2-byte limit + 8-byte base) */
    {
        arch_desc_t idt_desc = arch_sidt();
        uint16_t *idt_limit = (uint16_t *)phys_to_virt(TRAMP_DATA + 0x30);
        uint64_t *idt_base  = (uint64_t *)phys_to_virt(TRAMP_DATA + 0x32);
        *idt_limit = idt_desc.limit;
        *idt_base  = idt_desc.base;
    }

    arch_mfence();

    serial_puts("SMP: trampoline at 0x");
    serial_uint(TRAMP_PHYS);
    serial_puts(" (");
    serial_uint(ap_trampoline_bin_size);
    serial_puts(" bytes)\n");

    /* 3. INIT/SIPI sequence (Intel MP spec §B.4) */
    /* INIT/SIPI broadcast (Intel MP spec §B.4) */
    serial_puts("SMP: INIT/SIPI...");
    lapic_ipi_broadcast(ICR_INIT_ASSERT);
    lapic_ipi_broadcast(ICR_INIT_DEASSERT);
    timer_sleep_ms(10);
    lapic_ipi_broadcast(ICR_SIPI | (uint32_t)TRAMP_VECTOR);
    timer_sleep_ms(1);
    lapic_ipi_broadcast(ICR_SIPI | (uint32_t)TRAMP_VECTOR);
    serial_puts("sent\n");

    /* 4. Wait for APs to come alive (HLT instead of spin-wait) */
    uint64_t deadline = timer_ms() + 500;
    while (timer_ms() < deadline) {
        int alive = 0;
        for (int i = 0; i < SMP_MAX_CORES; i++)
            if (core_alive[i]) alive++;
        if (alive > total_cores) total_cores = alive;
        arch_halt();  /* sleep until next IRQ (LAPIC timer fires every 1ms) */
    }

    serial_puts("SMP: ");
    serial_uint((uint64_t)total_cores);
    serial_puts(" cores total\n");
    return total_cores - 1;
}

int smp_num_cores(void) { return total_cores; }

int smp_core_running(int core) {
    if (core < 0 || core >= SMP_MAX_CORES) return 0;
    return core_alive[core];
}

int smp_core_id(void) {
    volatile uint32_t *lapic_id = (volatile uint32_t *)LAPIC_ID;
    return (int)((*lapic_id >> 24) & 0xFF);
}

/* Send reschedule IPI (vector 0xFD) to a specific core.
 * Fire-and-forget: APIC write, no ACK wait, no locks.
 * Safe from any context (IRQ, kernel, RT-Core). */
void rt_wake(int core_id) {
    if (core_id < 0 || core_id >= SMP_MAX_CORES) return;
    if (!core_alive[core_id]) return;
    volatile uint32_t *icr_hi = (volatile uint32_t *)LAPIC_ICR_HI;
    volatile uint32_t *icr_lo = (volatile uint32_t *)LAPIC_ICR_LO;
    *icr_hi = ((uint32_t)core_id << 24);
    *icr_lo = 0x4000 | 0xFD;  /* Fixed delivery, vector 0xFD */
}
