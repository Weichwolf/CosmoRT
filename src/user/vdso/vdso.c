/* CosmoRT vDSO — user-space code page
 *
 * Compiled as a position-independent shared library, linked at fixed VA
 * VDSO_CODE_VA so PC-relative addressing of the data page (one page below)
 * works without runtime relocation. No GOT/PLT, no TLS, no globals.
 *
 * musl x86_64 looks up these symbols in the AT_SYSINFO_EHDR ELF:
 *   __vdso_clock_gettime (versioned LINUX_2.6)
 *   __vdso_getcpu        (versioned LINUX_2.6) — minimal stub
 *
 * Returns 0 on success, -ENOSYS to force syscall fallback for unsupported
 * clock IDs (musl handles this transparently).
 */

#include <stdint.h>

/* MUST match include/kernel/sys/vdso.h. Kept private here — the vDSO is
 * built standalone without kernel headers, so duplicate by hand. ABI break
 * if these diverge — _Static_assert on the kernel side guards size. */
struct vdso_data {
    uint32_t seq;
    uint32_t _pad0;
    uint32_t mult;
    uint32_t shift;
    uint64_t boot_tsc;
    int64_t  wall_time_offset_ns;
    uint64_t version;
    uint64_t _pad1[3];
};

#define VDSO_DATA_VA   0x7FFFEFFFE000ULL
#define VDSO_CODE_VA   0x7FFFEFFFF000ULL

/* Linux/musl clk_id values */
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7

#define ENOSYS  38
#define EINVAL  22

struct vdso_timespec { long tv_sec; long tv_nsec; };

/* Read the data page via fixed user-VA. Constant address — compiler
 * emits a direct mov, no GOT lookup. */
static inline const struct vdso_data *vdso_data(void) {
    return (const struct vdso_data *)VDSO_DATA_VA;
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    /* rdtsc serialized via lfence on entry to match the kernel-side
     * timer_tsc_now() which uses plain rdtsc. Slightly stricter, but
     * cheaper than rdtscp and safer for cross-CPU sequences. */
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* seqlock read: spin until the data is stable AND seq matches. Even seq
 * means writer is not in mid-update; matching pre/post means no update
 * raced with us. */
static inline void vdso_read_data(uint32_t *out_mult, uint32_t *out_shift,
                                  uint64_t *out_boot, int64_t *out_wall) {
    const struct vdso_data *d = vdso_data();
    uint32_t s1 = 0, s2 = 0;
    do {
        s1 = __atomic_load_n(&d->seq, __ATOMIC_ACQUIRE);
        if (s1 & 1) { __asm__ volatile("pause"); continue; }
        *out_mult  = d->mult;
        *out_shift = d->shift;
        *out_boot  = d->boot_tsc;
        *out_wall  = d->wall_time_offset_ns;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s2 = __atomic_load_n(&d->seq, __ATOMIC_ACQUIRE);
    } while (s1 != s2);
}

/* Compute monotonic ns from TSC delta + (mult, shift) scale.
 * Formula: ns = (tsc_delta * mult) >> shift.
 *
 * Naive: u64*u32 = up to 96-bit, but `tsc_delta * mult` can overflow 64
 * for long uptimes (mult ~ 2^28, tsc_delta > 2^36 -> overflow). Split
 * tsc_delta into high/low 32-bit halves and combine without precision
 * loss: low and high contributions are merged after the shift. shift is
 * always <= 32 from clocksource_calc_mult_shift (initial sft=32, only
 * decreases), so (32 - shift) >= 0. */
static inline uint64_t scale_tsc_to_ns(uint64_t tsc_delta, uint32_t mult, uint32_t shift) {
    uint64_t lo = tsc_delta & 0xFFFFFFFFULL;
    uint64_t hi = tsc_delta >> 32;
    uint64_t ns_lo = ((uint64_t)mult * lo) >> shift;
    uint64_t ns_hi = (hi * (uint64_t)mult) << (32 - shift);
    return ns_hi + ns_lo;
}

__attribute__((visibility("default")))
int __vdso_clock_gettime(int clk_id, struct vdso_timespec *ts) {
    if (!ts) return -EINVAL;

    switch (clk_id) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
        break;
    default:
        return -ENOSYS;     /* musl falls back to syscall */
    }

    uint32_t mult = 0, shift = 0;
    uint64_t boot = 0;
    int64_t  wall_off = 0;
    vdso_read_data(&mult, &shift, &boot, &wall_off);
    if (mult == 0) return -ENOSYS;     /* not yet calibrated */

    uint64_t tsc = rdtsc();
    if (tsc < boot) return -ENOSYS;    /* clock skew, fall back */
    uint64_t mono_ns = scale_tsc_to_ns(tsc - boot, mult, shift);

    int64_t out_ns = (int64_t)mono_ns;
    if (clk_id == CLOCK_REALTIME || clk_id == CLOCK_REALTIME_COARSE)
        out_ns += wall_off;

    int64_t sec  = out_ns / 1000000000;
    int64_t nsec = out_ns - sec * 1000000000;
    if (nsec < 0) { sec -= 1; nsec += 1000000000; }
    ts->tv_sec  = (long)sec;
    ts->tv_nsec = (long)nsec;
    return 0;
}

/* musl probes for __vdso_getcpu too. Single-CPU stub: report cpu=0,
 * node=0, ignore third arg. Linux-equivalent for non-NUMA. */
__attribute__((visibility("default")))
int __vdso_getcpu(unsigned *cpu, unsigned *node, void *unused) {
    (void)unused;
    if (cpu)  *cpu = 0;
    if (node) *node = 0;
    return 0;
}
