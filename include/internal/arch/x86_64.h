/* CosmoRT x86_64 Architecture Primitives
 *
 * All inline assembly lives here. src/kernel/ .c files must not
 * contain __asm__ — they call arch_*() instead.
 */
#ifndef COSMO_ARCH_X86_64_H
#define COSMO_ARCH_X86_64_H

#include <stdint.h>

/* --- TLB / Address Space ------------------------------------------ */

static inline void arch_flush_tlb(void) {
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3"
                     ::: "rax", "memory");
}

static inline void arch_invlpg(uint64_t va) {
    __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
}

static inline void arch_set_cr3(uint64_t phys) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(phys) : "memory");
}

/* --- Control Registers -------------------------------------------- */

static inline uint64_t arch_get_cr2(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static inline uint64_t arch_get_cr0(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline void arch_set_cr0(uint64_t v) {
    __asm__ volatile("mov %0, %%cr0" :: "r"(v));
}

static inline uint64_t arch_get_cr4(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void arch_set_cr4(uint64_t v) {
    __asm__ volatile("mov %0, %%cr4" :: "r"(v));
}

/* --- MSR ---------------------------------------------------------- */

static inline uint64_t arch_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void arch_wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" :: "c"(msr),
                     "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

static inline void arch_set_fs_base(uint64_t val) {
    arch_wrmsr(0xC0000100, val);   /* IA32_FS_BASE */
}

static inline uint64_t arch_get_fs_base(void) {
    return arch_rdmsr(0xC0000100);
}

static inline void arch_set_kernel_gs_base(uint64_t val) {
    arch_wrmsr(0xC0000102, val);   /* IA32_KERNEL_GS_BASE */
}

/* --- FPU / SSE state ---------------------------------------------- */

static inline void arch_fxsave(void *area) {
    __asm__ volatile("fxsave %0" : "=m"(*(char (*)[512])area));
}

static inline void arch_fxrstor(const void *area) {
    __asm__ volatile("fxrstor %0" :: "m"(*(const char (*)[512])area));
}

/* --- CPU hints / halt --------------------------------------------- */

static inline void arch_halt(void) {
    __asm__ volatile("sti; hlt");
}

static inline void arch_cli_halt(void) {
    __asm__ volatile("cli; hlt");
}

static inline void arch_pause(void) {
    __asm__ volatile("pause");
}

static inline void arch_sti(void) {
    __asm__ volatile("sti");
}

static inline void arch_cli(void) {
    __asm__ volatile("cli");
}

/* --- Barriers ----------------------------------------------------- */

static inline void arch_mfence(void) {
    __asm__ volatile("mfence" ::: "memory");
}

/* --- TSC ---------------------------------------------------------- */

static inline uint64_t arch_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* --- CPUID -------------------------------------------------------- */

static inline void arch_cpuid(uint32_t leaf,
                               uint32_t *eax, uint32_t *ebx,
                               uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf));
}

static inline void arch_cpuid_count(uint32_t leaf, uint32_t subleaf,
                                     uint32_t *eax, uint32_t *ebx,
                                     uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(subleaf));
}

/* --- RDRAND ------------------------------------------------------- */

static inline uint64_t arch_rdrand(void) {
    uint64_t r;
    __asm__ volatile("rdrand %0" : "=r"(r));
    return r;
}

static inline int arch_rdrand_checked(uint64_t *out) {
    uint64_t r;
    unsigned char ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(r), "=qm"(ok));
    *out = r;
    return ok;
}

/* --- I/O ports ---------------------------------------------------- */

static inline void arch_outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint8_t arch_inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void arch_outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %w1" :: "a"(val), "Nd"(port));
}

static inline uint32_t arch_inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile("inl %w1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* --- Descriptor tables -------------------------------------------- */

static inline void arch_lidt(const void *desc) {
    __asm__ volatile("lidt %0" :: "m"(*(const char (*)[10])desc));
}

static inline void arch_ltr(uint16_t sel) {
    __asm__ volatile("ltr %w0" :: "r"(sel));
}

typedef struct { uint16_t limit; uint64_t base; } __attribute__((packed)) arch_desc_t;

static inline arch_desc_t arch_sgdt(void) {
    arch_desc_t d;
    __asm__ volatile("sgdt %0" : "=m"(d));
    return d;
}

static inline arch_desc_t arch_sidt(void) {
    arch_desc_t d;
    __asm__ volatile("sidt %0" : "=m"(d));
    return d;
}

/* --- Stack pointer ------------------------------------------------ */

static inline uint64_t arch_get_rsp(void) {
    uint64_t sp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
    return sp;
}

/* --- Hyper-V hypercall -------------------------------------------- */

static inline uint64_t arch_hyperv_call(uint64_t control, uint64_t input_phys,
                                         uint64_t output_phys, void *hc_page) {
    uint64_t result;
    __asm__ volatile(
        "mov %1, %%rcx\n\t"
        "mov %2, %%rdx\n\t"
        "mov %3, %%r8\n\t"
        "call *%4\n\t"
        "mov %%rax, %0"
        : "=r"(result)
        : "r"(control), "r"(input_phys), "r"(output_phys), "r"(hc_page)
        : "rcx", "rdx", "r8", "rax", "memory"
    );
    return result;
}

#endif
