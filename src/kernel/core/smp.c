/* SMP — INIT/SIPI AP wakeup for x86_64 */

#include "config.h"
#include "core/smp.h"
#include "hw/serial.h"
#include "core/timer.h"
#include "core/percpu.h"
#include "memops.h"
#include "arch/arch.h"

#include "gen/ap_trampoline_bin.h"

#define LAPIC_ID      (0xFEE00020ULL + PHYS_OFFSET)
#define LAPIC_ICR_LO  (0xFEE00300ULL + PHYS_OFFSET)
#define LAPIC_ICR_HI  (0xFEE00310ULL + PHYS_OFFSET)

#define TRAMP_PHYS    0x8000ULL
#define TRAMP_VECTOR  (TRAMP_PHYS >> 12)
#define TRAMP_DATA    0x8F00ULL

#define ICR_INIT_ASSERT    0x0000C500
#define ICR_INIT_DEASSERT  0x00008500
#define ICR_SIPI           0x00000600
#define ICR_DELIVERY_PENDING (1 << 12)

#define TSC_SYNC_THRESHOLD_US 10

extern uint64_t pml4[];

static volatile int core_alive[SMP_MAX_CORES];
static volatile uint64_t core_boot_tsc[SMP_MAX_CORES];
static int total_cores = 1;

static uint8_t core_stacks[SMP_MAX_CORES][SMP_STACK_SIZE] __attribute__((aligned(16)));

static void (*user_entry_fn)(void);

#define LAPIC_SVR        (0xFEE000F0ULL + PHYS_OFFSET)
#define LAPIC_TPR        (0xFEE00080ULL + PHYS_OFFSET)
#define LAPIC_TIMER_REG  (0xFEE00320ULL + PHYS_OFFSET)
#define LAPIC_TIMER_INIT (0xFEE00380ULL + PHYS_OFFSET)
#define LAPIC_TIMER_DIV  (0xFEE003E0ULL + PHYS_OFFSET)
#define LAPIC_EOI_REG    (0xFEE000B0ULL + PHYS_OFFSET)

static void ap_main(void) {
    uint64_t cr0 = arch_get_cr0();
    cr0 &= ~(1ULL << 2);
    cr0 &= ~(1ULL << 3);
    cr0 |= (1ULL << 1);
    arch_set_cr0(cr0);
    uint64_t cr4 = arch_get_cr4();
    cr4 |= (1 << 9) | (1 << 10);

    uint32_t eax, ebx, ecx, edx;
    arch_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
    if (ebx & (1U << 7))  cr4 |= (1ULL << 20);
    if (ebx & (1U << 20)) cr4 |= (1ULL << 21);

    arch_set_cr4(cr4);
    arch_clac();

    *(volatile uint32_t *)LAPIC_SVR = 0x1FF;
    *(volatile uint32_t *)LAPIC_TPR = 0;
    *(volatile uint32_t *)LAPIC_TIMER_DIV = 0x03;
    *(volatile uint32_t *)LAPIC_TIMER_REG = 0x20020;
    *(volatile uint32_t *)LAPIC_TIMER_INIT = 100000;

    volatile uint32_t *lapic_id_reg = (volatile uint32_t *)LAPIC_ID;
    uint32_t apic_id = (*lapic_id_reg >> 24) & 0xFF;

    if (apic_id < SMP_MAX_CORES) {
        core_boot_tsc[apic_id] = arch_rdtsc();
        __sync_val_compare_and_swap(&core_alive[apic_id], 0, 1);
    }

    percpu_init_ap((int)apic_id);

    extern void tss_init_ap(void);
    tss_init_ap();

    extern void syscall_init(void);
    syscall_init();

    serial_puts("SMP: Core ");
    if (apic_id >= 10) serial_putchar('0' + (apic_id / 10));
    serial_putchar('0' + (apic_id % 10));
    serial_puts(" alive\n");

    if (user_entry_fn) user_entry_fn();

    for (;;) arch_halt();
}

static void lapic_ipi_broadcast(uint32_t icr_lo_val) {
    volatile uint32_t *icr_lo = (volatile uint32_t *)LAPIC_ICR_LO;
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

    uint8_t *tramp_dst = (uint8_t *)phys_to_virt(TRAMP_PHYS);
    kmemcpy(tramp_dst, ap_trampoline_bin, ap_trampoline_bin_size);

    volatile uint64_t *data = (volatile uint64_t *)phys_to_virt(TRAMP_DATA);
    data[0] = virt_to_phys(pml4);
    data[1] = (uint64_t)(uintptr_t)core_stacks;
    data[2] = ensure_high((uint64_t)(uintptr_t)ap_main);
    {
        arch_desc_t gdt_desc = arch_sgdt();
        uint16_t *gdt_limit = (uint16_t *)phys_to_virt(TRAMP_DATA + 0x18);
        uint64_t *gdt_base  = (uint64_t *)phys_to_virt(TRAMP_DATA + 0x1A);
        *gdt_limit = gdt_desc.limit;
        *gdt_base  = gdt_desc.base;
    }
    data[5] = SMP_STACK_SIZE;
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

    serial_puts("SMP: INIT/SIPI...");
    lapic_ipi_broadcast(ICR_INIT_ASSERT);
    lapic_ipi_broadcast(ICR_INIT_DEASSERT);
    timer_sleep_ms(10);
    lapic_ipi_broadcast(ICR_SIPI | (uint32_t)TRAMP_VECTOR);
    timer_sleep_ms(1);
    lapic_ipi_broadcast(ICR_SIPI | (uint32_t)TRAMP_VECTOR);
    serial_puts("sent\n");

    uint64_t deadline = timer_ms() + 500;
    while (timer_ms() < deadline) {
        int alive = 0;
        for (int i = 0; i < SMP_MAX_CORES; i++)
            if (core_alive[i]) alive++;
        if (alive > total_cores) total_cores = alive;
        arch_halt();
    }

    if (total_cores > 1 && timer_tsc_per_ms > 0) {
        uint64_t ref_tsc = 0;
        int ref_core = -1;
        for (int i = 1; i < SMP_MAX_CORES; i++) {
            if (!core_alive[i] || core_boot_tsc[i] == 0) continue;
            if (ref_core < 0) { ref_tsc = core_boot_tsc[i]; ref_core = i; }
        }
        if (ref_core >= 0) {
            uint64_t tsc_per_us = timer_tsc_per_ms / 1000;
            if (tsc_per_us == 0) tsc_per_us = 1;
            for (int i = ref_core + 1; i < SMP_MAX_CORES; i++) {
                if (!core_alive[i] || core_boot_tsc[i] == 0) continue;
                uint64_t a = core_boot_tsc[i], b = ref_tsc;
                uint64_t diff_us = ((a > b) ? a - b : b - a) / tsc_per_us;
                if (diff_us > (uint64_t)TSC_SYNC_THRESHOLD_US * 1000) {
                    serial_puts("TSC: WARNING — core ");
                    serial_uint((uint64_t)i);
                    serial_puts(" vs core ");
                    serial_uint((uint64_t)ref_core);
                    serial_puts(" offset ");
                    serial_uint(diff_us);
                    serial_puts("us\n");
                }
            }
        }
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

void rt_wake(int core_id) {
    if (core_id < 0 || core_id >= SMP_MAX_CORES) return;
    if (!core_alive[core_id]) return;
    volatile uint32_t *icr_hi = (volatile uint32_t *)LAPIC_ICR_HI;
    volatile uint32_t *icr_lo = (volatile uint32_t *)LAPIC_ICR_LO;
    *icr_hi = ((uint32_t)core_id << 24);
    *icr_lo = 0x4000 | 0xFD;
}
