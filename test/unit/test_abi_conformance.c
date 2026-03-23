/* P0 ABI Conformance Tests — register preservation, signal isolation,
 * TLS isolation, demand-zero, mprotect enforcement.
 * Reference: TESTSUITE.md sections 1, 2, 6, 10, 11. */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)

struct ksigaction_c {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

__attribute__((naked)) static void sig_restorer_c(void) {
    __asm__ volatile("mov $15, %%rax\n" "syscall\n" ::: "memory");
}

/* ══════════════════════════════════════════════════════════════════
 *  1. syscall-preserves-callee-saved
 * ══════════════════════════════════════════════════════════════════ */

/* Test register preservation in a fork'd child to avoid interfering
 * with the compiler's register allocation in the test function itself. */
static void test_syscall_preserves_callee_saved(void) {
    puts("\n[ABI: syscall preserves callee-saved]\n");

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Child: test register preservation across syscall.
         * Use rdi for result pointer — it's caller-saved and safe. */
        uint64_t results[6];
        register uint64_t *rp __asm__("rdi") = results;
        __asm__ volatile(
            /* Save rdi (result ptr) on stack first */
            "push %%rdi\n"
            /* Save compiler's callee-saved regs */
            "push %%rbx\n push %%rbp\n push %%r12\n"
            "push %%r13\n push %%r14\n push %%r15\n"
            /* Load test patterns */
            "movabs $0xAAAAAAAAAAAAAAAA, %%rbx\n"
            "movabs $0xBBBBBBBBBBBBBBBB, %%rbp\n"
            "movabs $0xCCCCCCCCCCCCCCCC, %%r12\n"
            "movabs $0xDDDDDDDDDDDDDDDD, %%r13\n"
            "movabs $0xEEEEEEEEEEEEEEEE, %%r14\n"
            "movabs $0xFFFFFFFFFFFFFFFF, %%r15\n"
            /* Syscall (getpid) — must preserve rbx,rbp,r12-r15 */
            "mov $39, %%eax\n" "syscall\n"
            /* Recover result pointer from stack (7 pushes deep) */
            "mov 48(%%rsp), %%rdi\n"
            /* Store results */
            "mov %%rbx, 0(%%rdi)\n"
            "mov %%rbp, 8(%%rdi)\n"
            "mov %%r12, 16(%%rdi)\n"
            "mov %%r13, 24(%%rdi)\n"
            "mov %%r14, 32(%%rdi)\n"
            "mov %%r15, 40(%%rdi)\n"
            /* Restore */
            "pop %%r15\n pop %%r14\n pop %%r13\n"
            "pop %%r12\n pop %%rbp\n pop %%rbx\n"
            "pop %%rdi\n"
            : "+r"(rp)
            :
            : "rax","rcx","r11","rsi","rdx","r10","r8","r9","memory"
        );
        int ok = (results[0] == 0xAAAAAAAAAAAAAAAAULL &&
                  results[1] == 0xBBBBBBBBBBBBBBBBULL &&
                  results[2] == 0xCCCCCCCCCCCCCCCCULL &&
                  results[3] == 0xDDDDDDDDDDDDDDDDULL &&
                  results[4] == 0xEEEEEEEEEEEEEEEEULL &&
                  results[5] == 0xFFFFFFFFFFFFFFFFULL);
        sc1(SYS_EXIT_GROUP, ok ? 0 : 1);
        __builtin_unreachable();
    }
    check("fork child for register test", pid > 0);
    if (pid > 0) {
        int st = 0;
        sc4(SYS_WAIT4, pid, (long)&st, 0, 0);
        check("callee-saved preserved", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
}
TEST("syscall-preserves-callee-saved", test_syscall_preserves_callee_saved);

/* ══════════════════════════════════════════════════════════════════
 *  2. syscall-preserves-rsp
 * ══════════════════════════════════════════════════════════════════ */

static void test_syscall_preserves_rsp(void) {
    puts("\n[ABI: syscall preserves RSP]\n");

    uint64_t rsp_before, rsp_after;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp_before));
    sc0(SYS_GETPID);
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp_after));

    puts("  rsp before=0x"); put_hex(rsp_before); puts("\n");
    puts("  rsp after =0x"); put_hex(rsp_after);  puts("\n");
    check("rsp preserved over syscall", rsp_before == rsp_after);
}
TEST("syscall-preserves-rsp", test_syscall_preserves_rsp);

/* ══════════════════════════════════════════════════════════════════
 *  3. signal-handler-register-isolation
 *     (nested signals must not leak registers)
 * ══════════════════════════════════════════════════════════════════ */

static volatile uint64_t iso_a_rbx_before, iso_a_rbx_after;
static volatile int iso_a_ok, iso_b_ran;

/* Handler B: trash RBX deliberately */
static void iso_handler_b(int sig) {
    (void)sig;
    __asm__ volatile("mov $0xBADBADBADBADBAD0, %%rbx" ::: "rbx");
    iso_b_ran = 1;
}

/* Handler A: save RBX, trigger B, check RBX restored after B returns */
static void iso_handler_a(int sig) {
    (void)sig;
    uint64_t my_rbx = 0xA1A1A1A1A1A1A1A1ULL;
    uint64_t after;
    __asm__ volatile("mov %0, %%rbx" : : "r"(my_rbx) : "rbx");

    /* Send SIGUSR2 to self — triggers handler B */
    sc2(SYS_KILL, sc0(SYS_GETPID), SIGUSR2);

    __asm__ volatile("mov %%rbx, %0" : "=r"(after) :: );
    iso_a_rbx_before = my_rbx;
    iso_a_rbx_after  = after;
    iso_a_ok = (after == my_rbx);
}

static void test_signal_register_isolation(void) {
    puts("\n[ABI: signal handler register isolation]\n");

    iso_a_ok = 0;
    iso_b_ran = 0;

    /* Install handler A for SIGUSR1 */
    struct ksigaction_c sa_a = {
        .handler  = (void *)iso_handler_a,
        .flags    = SA_RESTORER,
        .restorer = (void *)sig_restorer_c,
        .mask     = 0,
    };
    sc4(SYS_RT_SIGACTION, SIGUSR1, (long)&sa_a, 0, 8);

    /* Install handler B for SIGUSR2 */
    struct ksigaction_c sa_b = {
        .handler  = (void *)iso_handler_b,
        .flags    = SA_RESTORER,
        .restorer = (void *)sig_restorer_c,
        .mask     = 0,
    };
    sc4(SYS_RT_SIGACTION, SIGUSR2, (long)&sa_b, 0, 8);

    /* Fire SIGUSR1 */
    sc2(SYS_KILL, sc0(SYS_GETPID), SIGUSR1);

    check("handler B ran", iso_b_ran == 1);
    puts("  handler A rbx before=0x"); put_hex(iso_a_rbx_before); puts("\n");
    puts("  handler A rbx after =0x"); put_hex(iso_a_rbx_after);  puts("\n");
    check("handler A rbx not leaked from B", iso_a_ok);
}
TEST("signal-register-isolation", test_signal_register_isolation);

/* ══════════════════════════════════════════════════════════════════
 *  4. signal-redzone-128
 *     Red zone (128 bytes below RSP) preserved across signals
 * ══════════════════════════════════════════════════════════════════ */

static volatile int rz2_handler_ran;

static void rz2_handler(int sig) {
    (void)sig;
    rz2_handler_ran = 1;
}

static void test_signal_redzone_128(void) {
    puts("\n[ABI: signal red zone 128 bytes]\n");

    rz2_handler_ran = 0;

    struct ksigaction_c sa = {
        .handler  = (void *)rz2_handler,
        .flags    = SA_RESTORER,
        .restorer = (void *)sig_restorer_c,
        .mask     = 0,
    };
    sc4(SYS_RT_SIGACTION, SIGUSR1, (long)&sa, 0, 8);

    /* Place 8 canaries across the 128-byte red zone:
     * offsets -8, -24, -40, -56, -72, -88, -104, -120 */
    uint64_t c0, c1, c2, c3, c4, c5, c6, c7;
    uint64_t v0 = 0xCAFEBABE00000001ULL, v1 = 0xCAFEBABE00000002ULL;
    uint64_t v2 = 0xCAFEBABE00000003ULL, v3 = 0xCAFEBABE00000004ULL;
    uint64_t v4 = 0xCAFEBABE00000005ULL, v5 = 0xCAFEBABE00000006ULL;
    uint64_t v6 = 0xCAFEBABE00000007ULL, v7 = 0xCAFEBABE00000008ULL;

    __asm__ volatile(
        "movq %[v0],   -8(%%rsp)\n"
        "movq %[v1],  -24(%%rsp)\n"
        "movq %[v2],  -40(%%rsp)\n"
        "movq %[v3],  -56(%%rsp)\n"
        "movq %[v4],  -72(%%rsp)\n"
        "movq %[v5],  -88(%%rsp)\n"
        "movq %[v6], -104(%%rsp)\n"
        "movq %[v7], -120(%%rsp)\n"
        :: [v0]"r"(v0), [v1]"r"(v1), [v2]"r"(v2), [v3]"r"(v3),
           [v4]"r"(v4), [v5]"r"(v5), [v6]"r"(v6), [v7]"r"(v7)
        : "memory"
    );

    /* Signal delivery must skip 128 bytes */
    sc2(SYS_KILL, sc0(SYS_GETPID), SIGUSR1);

    __asm__ volatile(
        "movq   -8(%%rsp), %[c0]\n"
        "movq  -24(%%rsp), %[c1]\n"
        "movq  -40(%%rsp), %[c2]\n"
        "movq  -56(%%rsp), %[c3]\n"
        "movq  -72(%%rsp), %[c4]\n"
        "movq  -88(%%rsp), %[c5]\n"
        "movq -104(%%rsp), %[c6]\n"
        "movq -120(%%rsp), %[c7]\n"
        : [c0]"=r"(c0), [c1]"=r"(c1), [c2]"=r"(c2), [c3]"=r"(c3),
          [c4]"=r"(c4), [c5]"=r"(c5), [c6]"=r"(c6), [c7]"=r"(c7)
        :: "memory"
    );

    check("redzone handler ran", rz2_handler_ran == 1);
    check("redzone  -8 intact", c0 == v0);
    check("redzone -24 intact", c1 == v1);
    check("redzone -40 intact", c2 == v2);
    check("redzone -56 intact", c3 == v3);
    check("redzone -72 intact", c4 == v4);
    check("redzone -88 intact", c5 == v5);
    check("redzone -104 intact", c6 == v6);
    check("redzone -120 intact", c7 == v7);
}
TEST("signal-redzone-128", test_signal_redzone_128);

/* ══════════════════════════════════════════════════════════════════
 *  5. fork-tls-isolation
 *     Child's FS_BASE must be independent copy of parent's
 * ══════════════════════════════════════════════════════════════════ */

static void test_fork_tls_isolation(void) {
    puts("\n[ABI: fork TLS isolation]\n");

    uint64_t parent_fs = 0x7F0000000000ULL;
    sc2(SYS_ARCH_PRCTL, ARCH_SET_FS, parent_fs);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Child: read FS_BASE, exit with result */
        uint64_t fs;
        sc2(SYS_ARCH_PRCTL, ARCH_GET_FS, (long)&fs);
        sc1(SYS_EXIT_GROUP, (fs == parent_fs) ? 0 : 1);
        __builtin_unreachable();
    }

    check("fork succeeded", pid > 0);
    if (pid <= 0) return;

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));
    check_val("child FS_BASE matches parent", (long)WEXITSTATUS(wstatus), 0);

    /* Verify parent FS_BASE untouched */
    uint64_t fs_after;
    sc2(SYS_ARCH_PRCTL, ARCH_GET_FS, (long)&fs_after);
    puts("  parent fs after=0x"); put_hex(fs_after); puts("\n");
    check("parent FS_BASE preserved", fs_after == parent_fs);
}
TEST("fork-tls-isolation", test_fork_tls_isolation);

/* ══════════════════════════════════════════════════════════════════
 *  6. mmap-demand-zero
 *     Anonymous pages must be zeroed on first access
 * ══════════════════════════════════════════════════════════════════ */

static void test_mmap_demand_zero(void) {
    puts("\n[ABI: mmap demand zero]\n");

    long pg = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap succeeded", pg > 0);
    if (pg <= 0) return;

    /* Check all 4096 bytes are zero */
    volatile uint8_t *p = (volatile uint8_t *)pg;
    int nonzero = 0;
    for (int i = 0; i < 4096; i++)
        if (p[i] != 0) nonzero++;

    check_val("anonymous page all zeroes", nonzero, 0);

    /* Also test a larger mapping (multiple pages) — spot-check first
     * and last bytes of a 16-page region */
    long big = sc6(SYS_MMAP, 0, 65536, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("big mmap succeeded", big > 0);
    if (big > 0) {
        volatile uint8_t *bp = (volatile uint8_t *)big;
        check("big page[0] zero", bp[0] == 0);
        check("big page[last] zero", bp[65535] == 0);
        check("big page[mid] zero", bp[32768] == 0);
        sc2(SYS_MUNMAP, big, 65536);
    }

    sc2(SYS_MUNMAP, pg, 4096);
}
TEST("mmap-demand-zero", test_mmap_demand_zero);

/* ══════════════════════════════════════════════════════════════════
 *  7. mprotect-takes-effect
 *     mprotect(PROT_NONE) must make page inaccessible (SIGSEGV)
 *     mprotect back to PROT_READ must restore access + data
 * ══════════════════════════════════════════════════════════════════ */

static volatile int mprotect_sigsegv_caught;

static void mprotect_segv_handler(int sig) {
    (void)sig;
    mprotect_sigsegv_caught = 1;
    /* Skip the faulting instruction — jump past the read.
     * We modify RIP via the thread's register state:
     * Set rax to 0 and skip 3 bytes (mov (%rdi),%rax is 3 bytes). */
}

static void test_mprotect_takes_effect(void) {
    puts("\n[ABI: mprotect takes effect]\n");

    long pg = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap for mprotect", pg > 0);
    if (pg <= 0) return;

    *(volatile uint64_t *)pg = 0xCAFECAFEULL;

    /* Protect PROT_NONE — use fork to test, child accesses protected page */
    long r = sc3(SYS_MPROTECT, pg, 4096, PROT_NONE);
    check_val("mprotect PROT_NONE ok", r, 0);

    /* Fork child to test — child has fresh TLB (mov cr3 in fork) */
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Child: access PROT_NONE page → must get SIGSEGV → killed */
        volatile uint64_t v = *(volatile uint64_t *)pg;
        (void)v;
        /* If we reach here, PROT_NONE didn't work */
        sc1(SYS_EXIT_GROUP, 42);
        __builtin_unreachable();
    }
    check("fork for SIGSEGV test", pid > 0);
    if (pid > 0) {
        int wstatus = 0;
        sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
        /* Accept signal death (WIFSIGNALED) OR exit(128+11=139) — both indicate SIGSEGV */
        int sig_death = WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == 11;
        int exit_139  = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 139;
        int exit_42   = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 42;
        if (exit_42) {
            puts("  WARN: child survived PROT_NONE read (TLB not flushed?)\n");
        }
        check("PROT_NONE causes signal", sig_death || exit_139);
    }

    /* Restore read access — data must survive in parent */
    r = sc3(SYS_MPROTECT, pg, 4096, PROT_READ);
    check_val("mprotect PROT_READ ok", r, 0);
    check("data preserved after PROT_NONE->PROT_READ",
          *(volatile uint64_t *)pg == 0xCAFECAFEULL);

    sc2(SYS_MUNMAP, pg, 4096);
}
TEST("mprotect-takes-effect", test_mprotect_takes_effect);
