/* SMP — Wake 1..N Application Processors */
#include "config.h"

#include "smp.h"
#include "serial.h"
#include "timer.h"

/* Shared wake flags — one per possible AP (indexed by APIC ID) */
/* Defined in entry.asm for APs to poll */
extern volatile uint64_t ap_go;
extern volatile uint64_t ap_entry_addr;
extern volatile uint64_t ap_stack_ptr;
extern volatile uint64_t ap_cr3;

extern uint64_t pml4[];

/* Per-core state */
static volatile int core_alive[SMP_MAX_CORES]; /* accessed via __sync builtins */
static int total_cores = 1; /* BSP counts as 1 */

/* Per-core stacks (64KB each) */
static uint8_t core_stacks[SMP_MAX_CORES][SMP_STACK_SIZE] __attribute__((aligned(16)));

static void (*user_entry_fn)(void);

/* AP entry point — runs in higher half (ap_cr3 loaded by entry.asm) */
static void ap_main(void) {
    /* Determine which core we are (LAPIC via direct map) */
    volatile uint32_t *lapic_id_reg = (volatile uint32_t *)(0xFEE00020 + PHYS_OFFSET);
    uint32_t apic_id = (*lapic_id_reg >> 24) & 0xFF;

    if (apic_id < SMP_MAX_CORES) __sync_val_compare_and_swap(&core_alive[apic_id], 0, 1);

    serial_puts("SMP: Core ");
    serial_putchar('0' + (apic_id & 0xF));
    serial_puts(" alive\n");

    if (user_entry_fn) user_entry_fn();

    for (;;) __asm__ volatile("sti; hlt");
}

int smp_start_all(void (*entry_fn)(void)) {
    user_entry_fn = entry_fn;
    core_alive[0] = 1; /* BSP */

    /* Configure AP entry */
    ap_entry_addr = (uint64_t)(uintptr_t)ap_main;
    /* Use core 1's stack for now — first AP to wake */
    ap_stack_ptr = (uint64_t)(uintptr_t)(core_stacks[1] + sizeof(core_stacks[1]));
    /* AP must load kernel page tables for higher-half access */
    ap_cr3 = virt_to_phys(pml4);

    __asm__ volatile("mfence" ::: "memory");

    /* Wake all APs at once */
    ap_go = 1;

    /* Wait for APs */
    uint64_t deadline = timer_ms() + 500;
    while (timer_ms() < deadline) {
        __asm__ volatile("pause");
        /* Count alive cores */
        int alive = 0;
        for (int i = 0; i < SMP_MAX_CORES; i++) if (core_alive[i]) alive++;
        if (alive > total_cores) total_cores = alive;
    }

    serial_puts("SMP: ");
    serial_putchar('0' + total_cores);
    serial_puts(" cores total\n");
    return total_cores - 1; /* return number of APs started */
}

int smp_num_cores(void) { return total_cores; }

int smp_core_running(int core) {
    if (core < 0 || core >= SMP_MAX_CORES) return 0;
    return core_alive[core];
}

int smp_core_id(void) {
    volatile uint32_t *lapic_id = (volatile uint32_t *)(0xFEE00020 + PHYS_OFFSET);
    return (*lapic_id >> 24) & 0xFF;
}
