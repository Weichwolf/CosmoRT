/* CosmoRT TSS + SYSCALL/SYSRET MSR setup */

#include <stdint.h>
#include "hw/serial.h"
#include "config.h"
#include "core/smp.h"
#include "arch/arch.h"

#define IOPB_SIZE 8193
#define TSS_MAX_CORES 8

struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
    uint8_t  iopb[IOPB_SIZE];
} __attribute__((packed));

static struct tss per_core_tss[TSS_MAX_CORES];

void tss_allow_port(uint16_t port) {
    int byte = port / 8;
    int bit = port % 8;
    if (byte < IOPB_SIZE - 1)
        for (int c = 0; c < TSS_MAX_CORES; c++)
            per_core_tss[c].iopb[byte] &= ~(1 << bit);
}

void tss_allow_port_range(uint16_t start, uint16_t count) {
    for (uint16_t i = 0; i < count; i++) tss_allow_port(start + i);
}

void tss_set_rsp0(uint64_t rsp0) {
    int core = smp_core_id();
    if (core < TSS_MAX_CORES)
        per_core_tss[core].rsp0 = rsp0;
}

static inline void wrmsr(uint32_t msr, uint64_t val) { arch_wrmsr(msr, val); }
static inline uint64_t rdmsr(uint32_t msr) { return arch_rdmsr(msr); }

static void tss_load_for_core(int core) {
    if (core < 0 || core >= TSS_MAX_CORES) return;

    struct tss *t = &per_core_tss[core];

    uint8_t *p = (uint8_t *)t;
    for (int i = 0; i < (int)sizeof(struct tss); i++) p[i] = 0;

    t->iomap_base = __builtin_offsetof(struct tss, iopb);
    for (int i = 0; i < IOPB_SIZE; i++) t->iopb[i] = 0xFF;

    extern uint64_t tss_desc[];
    uint64_t base = ensure_high((uint64_t)(uintptr_t)t);
    uint16_t limit = sizeof(struct tss) - 1;

    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xFFFF);
    low |= (uint64_t)(base & 0xFFFF) << 16;
    low |= (uint64_t)((base >> 16) & 0xFF) << 32;
    low |= (uint64_t)0x89 << 40;
    low |= (uint64_t)((limit >> 16) & 0xF) << 48;
    low |= (uint64_t)((base >> 24) & 0xFF) << 56;

    uint64_t high = (base >> 32) & 0xFFFFFFFF;

    tss_desc[core * 2]     = low;
    tss_desc[core * 2 + 1] = high;

    uint16_t sel = (uint16_t)(0x30 + core * 0x10);
    arch_ltr(sel);
}

void tss_init(void) {
    tss_load_for_core(0);
    serial_puts("TSS: loaded (core 0)\n");
}

void tss_init_ap(void) {
    int core = smp_core_id();
    if (core > 0 && core < TSS_MAX_CORES) {
        tss_load_for_core(core);
        serial_puts("TSS: loaded (core ");
        if (core >= 10) serial_putchar('0' + core / 10);
        serial_putchar('0' + core % 10);
        serial_puts(")\n");
    }
}

void syscall_init(void) {
    uint64_t star = ((uint64_t)0x0018 << 48) | ((uint64_t)0x0008 << 32);
    wrmsr(0xC0000081, star);

    extern void syscall_entry_asm(void);
    wrmsr(0xC0000082, ensure_high((uint64_t)(uintptr_t)syscall_entry_asm));

    wrmsr(0xC0000084, 0x700);

    uint64_t efer = rdmsr(0xC0000080);
    efer |= 1;
    efer |= (1 << 11);
    wrmsr(0xC0000080, efer);

    serial_puts("syscall: LSTAR mode\n");
}
