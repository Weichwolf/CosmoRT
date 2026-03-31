/* test: FPU state preservation — reproduces musl fma/fmal/powf/remquol failures */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)

/*
 * Kernel must preserve all FPU state across syscalls and context switches.
 * - xmm0..xmm15: SSE registers
 * - x87 FPU control word (rounding mode etc.)
 * - MXCSR: SSE control/status
 * CosmoRT: clobbers FPU state, breaking musl math functions.
 *
 * Compiled with -mno-sse -mgeneral-regs-only, so all FPU access is via asm.
 */

/* XMM preservation across sched_yield */
static void test_xmm_across_syscall(void) {
    puts("\n[fpu/xmm-across-syscall]\n");

    double val = 3.141592653589793;
    double after = 0.0;

    __asm__ volatile(
        "movsd %0, %%xmm0\n"
        "movsd %0, %%xmm7\n"
        "movsd %0, %%xmm15\n"
        :: "m"(val)
    );

    sc0(SYS_SCHED_YIELD);

    __asm__ volatile("movsd %%xmm0, %0" : "=m"(after));
    /* Compare as uint64_t — no FP comparison available without SSE codegen */
    uint64_t a, b;
    __builtin_memcpy(&a, &val, 8);
    __builtin_memcpy(&b, &after, 8);
    check_val("xmm0 preserved", (long)b, (long)a);

    __asm__ volatile("movsd %%xmm7, %0" : "=m"(after));
    __builtin_memcpy(&b, &after, 8);
    check_val("xmm7 preserved", (long)b, (long)a);

    __asm__ volatile("movsd %%xmm15, %0" : "=m"(after));
    __builtin_memcpy(&b, &after, 8);
    check_val("xmm15 preserved", (long)b, (long)a);
}

/* XMM preservation across fork+wait (actual context switch) */
static void test_xmm_across_fork(void) {
    puts("\n[fpu/xmm-across-fork]\n");

    double val = 2.718281828459045;

    __asm__ volatile("movsd %0, %%xmm3" :: "m"(val));

    long pid = sc0(SYS_FORK);
    if (pid < 0) { fail("fork", 0); return; }

    if (pid == 0) {
        /* child: just exit — forces parent context switch */
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));

    double after;
    __asm__ volatile("movsd %%xmm3, %0" : "=m"(after));
    uint64_t a, b;
    __builtin_memcpy(&a, &val, 8);
    __builtin_memcpy(&b, &after, 8);
    check_val("xmm3 after fork", (long)b, (long)a);
}

/* x87 FPU control word preservation */
static void test_fpu_cw(void) {
    puts("\n[fpu/control-word]\n");

    uint16_t cw_before, cw_after;
    __asm__ volatile("fnstcw %0" : "=m"(cw_before));

    /* Set non-default: round toward zero (bits 10-11 = 11) */
    uint16_t new_cw = (cw_before & ~0x0C00u) | 0x0C00u;
    __asm__ volatile("fldcw %0" :: "m"(new_cw));

    sc0(SYS_SCHED_YIELD);

    __asm__ volatile("fnstcw %0" : "=m"(cw_after));

    puts("  cw set=0x"); put_hex(new_cw);
    puts("  cw got=0x"); put_hex(cw_after); puts("\n");

    check_val("fpu cw preserved", (long)cw_after, (long)new_cw);

    /* Restore original */
    __asm__ volatile("fldcw %0" :: "m"(cw_before));
}

/* MXCSR preservation */
static void test_mxcsr(void) {
    puts("\n[fpu/mxcsr]\n");

    uint32_t mxcsr_before, mxcsr_after;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr_before));

    /* Set round-toward-zero (bits 13-14 = 11) + flush-to-zero (bit 15) */
    uint32_t new_mxcsr = (mxcsr_before & ~0xE000u) | 0xE000u;
    __asm__ volatile("ldmxcsr %0" :: "m"(new_mxcsr));

    sc0(SYS_SCHED_YIELD);

    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr_after));

    puts("  mxcsr set=0x"); put_hex(new_mxcsr);
    puts("  mxcsr got=0x"); put_hex(mxcsr_after); puts("\n");

    check_val("mxcsr preserved", (long)mxcsr_after, (long)new_mxcsr);

    /* Restore original */
    __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr_before));
}

TEST("fpu/xmm-across-syscall", test_xmm_across_syscall);
TEST("fpu/xmm-across-fork", test_xmm_across_fork);
TEST("fpu/control-word", test_fpu_cw);
TEST("fpu/mxcsr", test_mxcsr);
