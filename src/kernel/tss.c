/* CosmoRT TSS + SYSCALL/SYSRET MSR setup
 *
 * TSS at GDT selector 0x30 (offset in new GDT layout).
 * SYSCALL via STAR/LSTAR/SFMASK MSRs.
 */

#include <stdint.h>
#include "serial.h"
#include "config.h"

/* TSS with I/O Permission Bitmap */
#define IOPB_SIZE 8193

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

static struct tss tss;

void tss_allow_port(uint16_t port) {
    int byte = port / 8;
    int bit = port % 8;
    if (byte < IOPB_SIZE - 1) tss.iopb[byte] &= ~(1 << bit);
}

void tss_allow_port_range(uint16_t start, uint16_t count) {
    for (uint16_t i = 0; i < count; i++) tss_allow_port(start + i);
}

void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

void tss_init(void) {
    /* Clear TSS */
    uint8_t *p = (uint8_t *)&tss;
    for (int i = 0; i < (int)sizeof(tss); i++) p[i] = 0;

    /* IOPB: all ports blocked by default */
    tss.iomap_base = __builtin_offsetof(struct tss, iopb);
    for (int i = 0; i < IOPB_SIZE; i++) tss.iopb[i] = 0xFF;

    /* Write TSS descriptor into GDT at offset 0x30 */
    extern uint64_t tss_desc[2];
    uint64_t base = ensure_high((uint64_t)(uintptr_t)&tss);
    uint16_t limit = sizeof(tss) - 1;

    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xFFFF);
    low |= (uint64_t)(base & 0xFFFF) << 16;
    low |= (uint64_t)((base >> 16) & 0xFF) << 32;
    low |= (uint64_t)0x89 << 40;                   /* type=9 (TSS), P=1 */
    low |= (uint64_t)((limit >> 16) & 0xF) << 48;
    low |= (uint64_t)((base >> 24) & 0xFF) << 56;

    uint64_t high = (base >> 32) & 0xFFFFFFFF;

    tss_desc[0] = low;
    tss_desc[1] = high;

    /* Load TR with TSS selector 0x30 */
    __asm__ volatile("ltr %w0" :: "r"((uint16_t)0x30));
    serial_puts("TSS: loaded\n");
}

void syscall_init(void) {
    /* IA32_STAR (0xC0000081):
     *   [47:32] = SYSCALL CS/SS base = 0x08 (kernel code)
     *   [63:48] = SYSRET CS/SS base  = 0x18
     *   SYSRET 64-bit: CS = (0x18+16)|3 = 0x2B, SS = (0x18+8)|3 = 0x23 */
    uint64_t star = ((uint64_t)0x0018 << 48) | ((uint64_t)0x0008 << 32);
    wrmsr(0xC0000081, star);

    /* IA32_LSTAR (0xC0000082): SYSCALL target RIP
     * Function pointer is identity-mapped (EFI relocation).
     * Add PHYS_OFFSET for direct-map address (valid in user PML4). */
    extern void syscall_entry_asm(void);
    wrmsr(0xC0000082, ensure_high((uint64_t)(uintptr_t)syscall_entry_asm));

    /* IA32_SFMASK (0xC0000084): RFLAGS bits to clear on SYSCALL
     * Clear IF (0x200), TF (0x100), DF (0x400) */
    wrmsr(0xC0000084, 0x700);

    /* Enable SYSCALL/SYSRET (IA32_EFER bit 0 = SCE) + NX bit (bit 11 = NXE) */
    uint64_t efer = rdmsr(0xC0000080);
    efer |= 1;          /* SCE */
    efer |= (1 << 11);  /* NXE — required for PTE_NX enforcement */
    wrmsr(0xC0000080, efer);

    serial_puts("syscall: LSTAR mode\n");
}
