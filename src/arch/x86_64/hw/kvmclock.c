/* KVM pvclock clocksource (rating 400).
 *
 * Host publishes a versioned pvclock_vcpu_time_info struct in guest memory
 * via MSR_KVM_SYSTEM_TIME_NEW. Readers take a sequence-lock snapshot
 * (version, tsc_timestamp, system_time, scale, shift), then compose
 * ns without a VMEXIT. Linux equivalent: arch/x86/kernel/kvmclock.c.
 *
 * CosmoRT is currently SMP=1, so one pvclock page suffices. Re-enabling
 * after migration / suspend is not implemented — relevant only when the
 * kernel learns to resume.
 */

#include "hw/kvmclock.h"
#include "core/clocksource.h"
#include "hw/serial.h"
#include "cosmort.h"
#include "arch/arch.h"

#define KVMCLOCK_CS_NAME        "kvm-clock"
#define KVMCLOCK_CS_RATING      CLOCKSOURCE_RATING_KVM_PVCLOCK
#define KVMCLOCK_CS_MULT        1u
#define KVMCLOCK_CS_SHIFT       0u
#define KVMCLOCK_PAGE_BYTES     4096u
#define KVMCLOCK_VERSION_UPDATE 0x1u

static struct pvclock_vcpu_time_info *kvm_pvti;

static inline int kvmclock_detect_cpuid(void) {
    uint32_t eax, ebx, ecx, edx;
    arch_cpuid(KVM_CPUID_SIGNATURE, &eax, &ebx, &ecx, &edx);
    if (ebx != KVM_SIGNATURE_EBX) return 0;
    if (ecx != KVM_SIGNATURE_ECX) return 0;
    if (edx != KVM_SIGNATURE_EDX) return 0;

    arch_cpuid(KVM_CPUID_FEATURES, &eax, &ebx, &ecx, &edx);
    return (eax & (1u << KVM_FEATURE_CLOCKSOURCE2)) ? 1 : 0;
}

int kvmclock_available(void) {
    return kvmclock_detect_cpuid();
}

uint64_t kvmclock_compose_ns(const struct pvclock_vcpu_time_info *ti,
                             uint64_t tsc) {
    uint64_t delta = tsc - ti->tsc_timestamp;
    if (ti->tsc_shift >= 0)
        delta <<= ti->tsc_shift;
    else
        delta >>= (uint8_t)(-ti->tsc_shift);

    __uint128_t wide = (__uint128_t)delta * (uint64_t)ti->tsc_to_system_mul;
    uint64_t offset = (uint64_t)(wide >> 32);
    return ti->system_time + offset;
}

uint64_t kvmclock_read_from(const struct pvclock_vcpu_time_info *ti) {
    if (!ti) return 0;

    struct pvclock_vcpu_time_info snap = { 0 };
    uint32_t v1;
    uint64_t tsc = 0;

    do {
        v1 = ((const volatile struct pvclock_vcpu_time_info *)ti)->version;
        if (v1 & KVMCLOCK_VERSION_UPDATE) {
            arch_pause();
            continue;
        }
        arch_mfence();
        snap = *ti;
        tsc  = arch_rdtsc();
        arch_mfence();
    } while (((const volatile struct pvclock_vcpu_time_info *)ti)->version != v1);

    return kvmclock_compose_ns(&snap, tsc);
}

uint64_t kvmclock_read_ns(void) {
    if (!kvm_pvti) return 0;
    return kvmclock_read_from(kvm_pvti);
}

const struct pvclock_vcpu_time_info *kvmclock_pvti(void) {
    return kvm_pvti;
}

static uint64_t kvmclock_cs_read(void) {
    return kvmclock_read_ns();
}

static struct clocksource kvmclock_cs = {
    .name   = KVMCLOCK_CS_NAME,
    .rating = KVMCLOCK_CS_RATING,
    .read   = kvmclock_cs_read,
    .mask   = CLOCKSOURCE_MASK_64,
    .mult   = KVMCLOCK_CS_MULT,
    .shift  = KVMCLOCK_CS_SHIFT,
    .flags  = CLOCKSOURCE_FLAG_CONTINUOUS
            | CLOCKSOURCE_FLAG_VALID_FOR_HRES
            | CLOCKSOURCE_FLAG_SUSPEND_NONSTOP,
};

__attribute__((cold))
void kvmclock_init(void) {
    if (kvm_pvti) return;
    if (!kvmclock_detect_cpuid()) return;

    void *virt;
    uint64_t phys;
    if (cosmo_dma_alloc(KVMCLOCK_PAGE_BYTES, &virt, &phys) < 0) {
        serial_puts("kvmclock: pvti alloc failed\n");
        return;
    }

    kvm_pvti = (struct pvclock_vcpu_time_info *)virt;

    arch_wrmsr(MSR_KVM_SYSTEM_TIME_NEW, phys | KVM_SYSTEM_TIME_ENABLE);

    clocksource_register(&kvmclock_cs);
    serial_puts("kvm-clock: registered (rating 400)\n");
}
