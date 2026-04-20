/* x86_64 feature detection via CPUID.
 *
 * Called once at boot. Populates global feature flags consumed by the
 * memory-ops path and entropy code. Kernel has no direct CPUID access -
 * aarch64 uses ID_AA64* system registers, different interface.
 */

#include "memops.h"
#include "arch/arch.h"

int memops_has_erms   = 0;
int memops_has_avx2   = 0;
int memops_has_rdrand = 0;

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
