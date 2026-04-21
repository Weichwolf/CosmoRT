/* CosmoRT KVM pvclock Clocksource
 *
 * KVM paravirtual clocksource: host publishes a versioned
 * pvclock_vcpu_time_info struct in guest memory; readers compose ns from
 * a TSC snapshot with host-provided scale/shift. No VMEXIT on the hot
 * path — pure memory + rdtsc.
 *
 * Layout and MSR ABI are Linux-kernel compatible (see
 * Documentation/virt/kvm/msr.rst, arch/x86/include/asm/pvclock-abi.h).
 */
#ifndef HW_KVMCLOCK_H
#define HW_KVMCLOCK_H

#include <stdint.h>

/* KVM-Hypervisor CPUID leaves (matches arch/x86/include/uapi/asm/kvm_para.h). */
#define KVM_CPUID_SIGNATURE        0x40000000u
#define KVM_CPUID_FEATURES         0x40000001u

/* ASCII signature returned in EBX:ECX:EDX at leaf 0x40000000: "KVMKVMKVM\0\0\0". */
#define KVM_SIGNATURE_EBX          0x4b4d564bu   /* "KVMK" */
#define KVM_SIGNATURE_ECX          0x564b4d56u   /* "VMKV" */
#define KVM_SIGNATURE_EDX          0x0000004du   /* "M\0\0\0" */

/* Feature bits at leaf 0x40000001:EAX. */
#define KVM_FEATURE_CLOCKSOURCE           0
#define KVM_FEATURE_CLOCKSOURCE2          3
#define KVM_FEATURE_CLOCKSOURCE_STABLE_BIT 24

/* KVM pvclock MSRs (new/current, present since KVM_FEATURE_CLOCKSOURCE2). */
#define MSR_KVM_WALL_CLOCK_NEW     0x4b564d00u
#define MSR_KVM_SYSTEM_TIME_NEW    0x4b564d01u

/* Enable bit for MSR_KVM_SYSTEM_TIME_NEW (phys addr | KVM_SYSTEM_TIME_ENABLE). */
#define KVM_SYSTEM_TIME_ENABLE     0x1u

/* pvclock flags (bit positions inside pvclock_vcpu_time_info.flags). */
#define PVCLOCK_TSC_STABLE_BIT     (1u << 0)
#define PVCLOCK_GUEST_STOPPED      (1u << 1)

/* Linux pvclock ABI — exact layout of the 32-byte vcpu time-info struct.
 * See arch/x86/include/asm/pvclock-abi.h. Field order and sizes are ABI and
 * must never change. */
struct pvclock_vcpu_time_info {
    uint32_t version;
    uint32_t pad0;
    uint64_t tsc_timestamp;
    uint64_t system_time;       /* ns since VM start */
    uint32_t tsc_to_system_mul;
    int8_t   tsc_shift;
    uint8_t  flags;
    uint8_t  pad[2];
} __attribute__((packed));

_Static_assert(sizeof(struct pvclock_vcpu_time_info) == 32,
               "pvclock_vcpu_time_info must match KVM ABI (32 bytes)");

/* Wall-clock ABI companion struct, published via MSR_KVM_WALL_CLOCK_NEW.
 * Currently unused by the read path but part of the public ABI. */
struct pvclock_wall_clock {
    uint32_t version;
    uint32_t sec;
    uint32_t nsec;
} __attribute__((packed));

_Static_assert(sizeof(struct pvclock_wall_clock) == 12,
               "pvclock_wall_clock must match KVM ABI (12 bytes)");

/* Probe KVM-Hypervisor signature + CLOCKSOURCE2 feature. Returns 1 iff
 * the host advertises the new pvclock MSR pair. No side effects. */
int  kvmclock_available(void);

/* Allocate the per-vCPU pvclock page, enable MSR_KVM_SYSTEM_TIME_NEW,
 * register a clocksource rating 400. No-op if kvmclock_available()==0 or
 * if the underlying page allocation fails. Idempotent. */
void kvmclock_init(void);

/* Full sequence-locked read against the live pvclock page.
 * Returns ns since VM start, or 0 when the page is not yet mapped. */
uint64_t kvmclock_read_ns(void);

/* Pure-function variant for tests: compose ns from a caller-supplied
 * pvclock struct and a TSC value. Implements the Linux formula:
 *   delta = tsc - ti->tsc_timestamp
 *   delta = ti->tsc_shift >= 0 ? delta << ti->tsc_shift
 *                              : delta >> -ti->tsc_shift
 *   ns    = ((delta * ti->tsc_to_system_mul) >> 32) + ti->system_time
 * Does NOT consult ti->version (caller drives the sequence lock). */
uint64_t kvmclock_compose_ns(const struct pvclock_vcpu_time_info *ti,
                             uint64_t tsc);

/* Sequence-locked read over a caller-supplied struct. Uses arch_rdtsc()
 * to sample TSC inside the lock. Exposed for tests that want to inject
 * a fake pvclock page without touching real MSRs. */
uint64_t kvmclock_read_from(const struct pvclock_vcpu_time_info *ti);

/* Pointer to the live pvclock_vcpu_time_info (NULL before init or when
 * kvmclock is not available). Tests peek the rating/flags via this. */
const struct pvclock_vcpu_time_info *kvmclock_pvti(void);

#endif
