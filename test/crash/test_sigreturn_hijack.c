/* Crash test: SROP — forge signal frame with kernel RIP/RSP, call rt_sigreturn.
 * Kernel must reject or kill the child. Never execute kernel code. */
#include "ktest.h"

#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)
#define WIFEXITED(s)    (((s) & 0x7F) == 0)

#define SIGSEGV  11

/* Linux x86_64 signal frame layout (must match kernel's sig_ucontext_t).
 * Offsets from RSP after signal delivery:
 *   [RSP+0]   = return address (restorer)
 *   [RSP+8]   = siginfo_t (128 bytes)
 *   [RSP+136] = ucontext_t (936 bytes)
 *     uc_flags    = +0   (8)
 *     uc_link     = +8   (8)
 *     uc_stack    = +16  (24)
 *     uc_mcontext = +40  (256)
 *       gregs     = +40  (184 = 23 * uint64_t)
 *         greg indices: R8=0..RSP=15, RIP=16, EFL=17, CSGSFS=18
 */
/* rt_sigreturn reads ucontext from (user_rsp - 8 + SIGFRAME_OFF_UCONTEXT).
 * SIGFRAME_OFF_UCONTEXT = 8 + 128 = 136. So uc_addr = user_rsp + 128.
 * Since we set RSP to `frame`, ucontext is at frame + 128. */
#define UC_OFF       128    /* ucontext offset from user_rsp (frame ptr) */
#define GREGS_OFF    (UC_OFF + 40)  /* uc_mcontext.gregs inside ucontext */
#define GREG_RSP     15
#define GREG_RIP     16
#define GREG_EFL     17
#define GREG_CSGSFS  18
#define FRAME_SIZE   1088   /* 8 + 128 + 936 + 16 (trampoline padded) */

/* Trampoline: mov rax, 15; syscall */
static const uint8_t trampoline[] = {
    0x48, 0xc7, 0xc0, 0x0f, 0x00, 0x00, 0x00, /* mov rax, 15 */
    0x0f, 0x05                                   /* syscall */
};

/* Try one SROP variant: forge frame with given rip/rsp, invoke rt_sigreturn.
 * Runs in a forked child. Returns: parent check result via wait status. */
static void try_srop(const char *desc, uint64_t evil_rip, uint64_t evil_rsp) {
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Child: build fake signal frame on mmap'd memory, pivot RSP, sigreturn */
        long frame = sc6(SYS_MMAP, 0, 8192, PROT_RW, MAP_PRIV_ANON, -1, 0);
        if (frame < 0)
            sc1(SYS_EXIT_GROUP, 99);

        /* Zero the frame */
        volatile uint8_t *p = (volatile uint8_t *)frame;
        for (int i = 0; i < FRAME_SIZE; i++) p[i] = 0;

        /* Return address = trampoline (won't be used, sigreturn doesn't return) */
        *(uint64_t *)(frame + 0) = 0;

        /* gregs: set RIP and RSP */
        uint64_t *gregs = (uint64_t *)(frame + GREGS_OFF);
        gregs[GREG_RIP] = evil_rip;
        gregs[GREG_RSP] = evil_rsp ? evil_rsp : (uint64_t)(frame + 4096); /* valid user RSP if not testing RSP */
        gregs[GREG_EFL] = 0x202;  /* IF set */
        gregs[GREG_CSGSFS] = 0x33; /* user CS */

        /* Pivot RSP to our fake frame and invoke rt_sigreturn */
        __asm__ volatile(
            "mov %0, %%rsp\n"
            "mov $15, %%rax\n"
            "syscall\n"
            :: "r"(frame)
            : "memory"
        );
        __builtin_unreachable();
    }
    check("fork for " , pid > 0);
    if (pid < 0) return;

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);

    /* Child must be dead: either killed by signal or exited with error.
     * It must NOT have successfully executed kernel code. */
    int ok = WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0);
    if (ok) {
        pass(desc);
    } else {
        puts("  FAIL  "); puts(desc);
        puts(" (status="); put_int(status); puts(")\n");
        failures++;
    }
}

static void test_sigreturn_hijack(void) {
    puts("\n[Sigreturn Hijack (SROP)]\n");

    /* Variant 1: RIP = canonical kernel address */
    try_srop("srop: rip=kernel_high", 0xFFFFFFFF80000000ULL, 0);

    /* Variant 2: RIP = non-canonical address */
    try_srop("srop: rip=non_canonical", 0xDEAD000000000000ULL, 0);

    /* Variant 3: RIP = first kernel address */
    try_srop("srop: rip=kern_boundary", 0x800000000000ULL, 0);

    /* Variant 4: RIP = NULL */
    try_srop("srop: rip=null", 0, 0);

    /* Variant 5: RSP = kernel space */
    try_srop("srop: rsp=kernel", 0x401000ULL /* plausible user RIP */, 0xFFFF800000000000ULL);

    /* Variant 6: both RIP and RSP in kernel space */
    try_srop("srop: rip+rsp=kernel", 0xFFFFFFFF80000000ULL, 0xFFFF800000001000ULL);
}

CRASH_TEST("crash/sigreturn_hijack", test_sigreturn_hijack);
