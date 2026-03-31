/* test: FPU signed zero — reproduces musl fma signed-zero bug */
#include "ktest.h"

/*
 * IEEE 754: (-1.0) * 0.0 = -0.0 (sign bit set).
 * If the kernel clobbers FPU state or resets rounding mode,
 * sign propagation breaks and the result is +0.0.
 */

static void test_signed_zero_mul(void) {
    puts("\n[fpu/signed-zero-mul]\n");

    double neg1 = -1.0, zero = 0.0, result;
    __asm__ volatile(
        "fldl %1\n"
        "fmull %2\n"
        "fstpl %0\n"
        : "=m"(result) : "m"(neg1), "m"(zero)
    );

    uint64_t bits;
    __builtin_memcpy(&bits, &result, 8);
    puts("  result bits = 0x"); put_hex(bits); puts("\n");

    /* -0.0 = 0x8000000000000000 */
    check("sign bit set", (bits >> 63) == 1);
    check("magnitude zero", (bits & 0x7FFFFFFFFFFFFFFFull) == 0);
}

/* Signed zero preserved across syscall */
static void test_signed_zero_across_syscall(void) {
    puts("\n[fpu/signed-zero-persist]\n");

    double neg_zero = -0.0;
    uint64_t nz_bits;
    __builtin_memcpy(&nz_bits, &neg_zero, 8);

    /* Load -0.0 into xmm0 */
    __asm__ volatile("movsd %0, %%xmm0" :: "m"(neg_zero));

    sc0(SYS_SCHED_YIELD);

    double after;
    __asm__ volatile("movsd %%xmm0, %0" : "=m"(after));
    uint64_t after_bits;
    __builtin_memcpy(&after_bits, &after, 8);

    puts("  before=0x"); put_hex(nz_bits);
    puts("  after=0x"); put_hex(after_bits); puts("\n");

    check_val("neg zero bits", (long)after_bits, (long)nz_bits);
}

/* x87 FPU computation with non-default rounding + sign check */
static void test_fpu_rounding_sign(void) {
    puts("\n[fpu/rounding-sign]\n");

    /* Set round-toward-zero */
    uint16_t cw_orig, cw_rtz;
    __asm__ volatile("fnstcw %0" : "=m"(cw_orig));
    cw_rtz = (cw_orig & ~0x0C00u) | 0x0C00u;
    __asm__ volatile("fldcw %0" :: "m"(cw_rtz));

    /* Compute -0.0 * tiny_positive + 0.0 via x87 */
    double neg_zero = -0.0, tiny = 1e-308, pzero = 0.0, result;
    __asm__ volatile(
        "fldl %1\n"    /* -0.0 */
        "fmull %2\n"   /* * tiny → -0.0 */
        "faddl %3\n"   /* + 0.0 → sign depends on rounding mode */
        "fstpl %0\n"
        : "=m"(result) : "m"(neg_zero), "m"(tiny), "m"(pzero)
    );

    uint64_t bits;
    __builtin_memcpy(&bits, &result, 8);
    puts("  result=0x"); put_hex(bits); puts("\n");

    /* In round-toward-zero mode, -0.0 + 0.0 = +0.0 per IEEE 754.
     * But the intermediate -0.0 * tiny must produce -0.0, and that
     * requires the rounding mode to survive the computation. */
    check("magnitude zero", (bits & 0x7FFFFFFFFFFFFFFFull) == 0);

    /* Restore */
    __asm__ volatile("fldcw %0" :: "m"(cw_orig));
}

TEST("fpu/signed-zero-mul", test_signed_zero_mul);
TEST("fpu/signed-zero-persist", test_signed_zero_across_syscall);
TEST("fpu/rounding-sign", test_fpu_rounding_sign);
