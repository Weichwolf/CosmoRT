/* x86_64 CPU feature detection and FPU/XSAVE boot init.
 *
 * All inherently x86 bits: CPUID, CR0/CR4 programming, XSETBV.
 * Kernel calls these via arch_fpu_boot_init/memops_init and never
 * touches the raw instructions.
 */

#include "memops.h"
#include "arch/arch.h"
#include "hw/serial.h"

int memops_has_erms   = 0;
int memops_has_avx2   = 0;
int memops_has_rdrand = 0;

/* XSAVE state - set at boot, read everywhere via include/kernel/arch/x86_64.h */
uint64_t xsave_xcr0 = XSTATE_FP | XSTATE_SSE;  /* default: x87+SSE */
uint32_t xsave_size = 512;                     /* default: FXSAVE */

__attribute__((cold))
void memops_init(void) {
    uint32_t eax, ebx, ecx, edx;

    /* CPUID leaf 7: ERMS (EBX bit 9), AVX2 (EBX bit 5) */
    arch_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
    if (ebx & (1u << 9)) memops_has_erms = 1;

    /* CPUID leaf 1: RDRAND (ECX bit 30) */
    arch_cpuid(1, &eax, &ebx, &ecx, &edx);
    if (ecx & (1u << 30)) memops_has_rdrand = 1;
}

/* SSE/SSE2 enable, SMEP/SMAP detect, XSAVE/FXSAVE + XCR0 programming.
 * Runs once from the BSP boot path. */
__attribute__((cold))
void hal_cpu_fpu_boot_init(void) {
    uint64_t cr0 = arch_get_cr0();
    cr0 &= ~(1ULL << 2);  /* clear CR0.EM (no x87 emulation) */
    cr0 &= ~(1ULL << 3);  /* clear CR0.TS (allow FPU/SSE without #NM) */
    cr0 |=  (1ULL << 1);  /* set CR0.MP (monitor coprocessor) */
    arch_set_cr0(cr0);

    uint64_t cr4 = arch_get_cr4();
    cr4 |= (1 << 9) | (1 << 10); /* CR4.OSFXSR + CR4.OSXMMEXCPT */

    /* SMEP + SMAP: prevent kernel from executing/accessing user pages.
     * CPUID.07H:EBX bit 7 = SMEP, bit 20 = SMAP. */
    uint32_t eax, ebx, ecx, edx;
    arch_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
    if (ebx & (1U << 7)) {
        cr4 |= (1ULL << 20);  /* CR4.SMEP */
        serial_puts("sec: SMEP enabled\n");
    }
    if (ebx & (1U << 20)) {
        cr4 |= (1ULL << 21);  /* CR4.SMAP */
        serial_puts("sec: SMAP enabled\n");
    }

    /* XSAVE: detect via CPUID.01H:ECX bit 26 (OSXSAVE-capable).
     * If present: enable CR4.OSXSAVE, query supported components via
     * CPUID.0DH, configure XCR0, read save area size.
     * Like Linux: eager FPU, XSAVE on every context switch. */
    arch_cpuid(1, &eax, &ebx, &ecx, &edx);
    if (ecx & (1U << 26)) {
        cr4 |= (1ULL << 18);  /* CR4.OSXSAVE */
        arch_set_cr4(cr4);

        uint32_t x_eax, x_ebx, x_ecx, x_edx;
        arch_cpuid_count(0x0D, 0, &x_eax, &x_ebx, &x_ecx, &x_edx);
        uint64_t hw_xcr0 = ((uint64_t)x_edx << 32) | x_eax;

        uint64_t want = XSTATE_FP | XSTATE_SSE;
        if (hw_xcr0 & XSTATE_AVX) want |= XSTATE_AVX;
        if ((hw_xcr0 & XSTATE_AVX512_OP) &&
            (hw_xcr0 & XSTATE_AVX512_HI) &&
            (hw_xcr0 & XSTATE_AVX512_EX))
            want |= XSTATE_AVX512_OP | XSTATE_AVX512_HI | XSTATE_AVX512_EX;

        arch_xsetbv(0, want);
        xsave_xcr0 = want;

        arch_cpuid_count(0x0D, 0, &x_eax, &x_ebx, &x_ecx, &x_edx);
        xsave_size = x_ebx;
        if (xsave_size > XSAVE_MAX_SIZE) xsave_size = XSAVE_MAX_SIZE;
        if (xsave_size < 512) xsave_size = 512;

        serial_puts("xsave: enabled (");
        if (want & XSTATE_AVX512_OP) serial_puts("AVX-512");
        else if (want & XSTATE_AVX)  serial_puts("AVX");
        else                         serial_puts("SSE");
        serial_puts(")\n");
    } else {
        xsave_xcr0 = XSTATE_FP | XSTATE_SSE;
        xsave_size = 512;
        arch_set_cr4(cr4);
        serial_puts("fpu: FXSAVE (no XSAVE)\n");
    }

    /* AC flag clear (CLAC) - default kernel state */
    arch_clac();
}
