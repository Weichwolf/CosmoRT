/* Tests for three signal-delivery bugs in CosmoRT.
 * All three FAIL with current code, PASS after fix. */
#include "ktest.h"

struct ksigaction_b {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

__attribute__((naked)) static void sig_restorer_b(void) {
    __asm__ volatile("mov $15, %%rax\n" "syscall\n" ::: "memory");
}

/* ── BUG-SIG1: Missing 128-byte Red-Zone subtraction ──
 *
 * deliver_signal() places the signal frame at the interrupted RSP
 * without subtracting 128 bytes for the AMD64 red zone.
 * The signal frame (siginfo + ucontext ≈ hundreds of bytes) stomps
 * data the interrupted code stored below RSP.
 */

static volatile int rz_handler_ran = 0;

static void redzone_handler(int sig) {
    (void)sig;
    rz_handler_ran = 1;
}

static void test_sig_redzone(void) {
    puts("\n[SIG-BUG1: Red Zone]\n");

    rz_handler_ran = 0;

    struct ksigaction_b sa = {
        .handler  = (void *)redzone_handler,
        .flags    = SA_RESTORER,
        .restorer = (void *)sig_restorer_b,
        .mask     = 0,
    };
    sc4(SYS_RT_SIGACTION, SIGUSR1, (long)&sa, 0, 8);

    uint64_t canary = 0xDEADBEEFCAFEBABEull;
    uint64_t recovered = 0;

    /* Write canary into red zone, send signal, read back.
     * Using RSP-120 (well inside the 128-byte zone). */
    __asm__ volatile(
        "movq %0, -120(%%rsp)\n"
        :: "r"(canary) : "memory"
    );

    sc2(SYS_KILL, sc0(SYS_GETPID), SIGUSR1);

    __asm__ volatile(
        "movq -120(%%rsp), %0\n"
        : "=r"(recovered) :: "memory"
    );

    check("redzone handler ran", rz_handler_ran == 1);
    puts("  canary expected=0x"); put_hex(canary); puts("\n");
    puts("  canary got=0x");      put_hex(recovered); puts("\n");
    check("redzone canary intact", recovered == canary);
}
TEST("sig-bug1-redzone", test_sig_redzone);

/* ── BUG-SIG2: Wrong RSP alignment in signal handler ──
 *
 * The kernel sets RSP ≡ 0 (mod 16) before entering the handler.
 * Correct: RSP ≡ 8 (mod 16), simulating a CALL that pushed a
 * return address onto a 16-aligned stack.
 *
 * We detect this by reading RSP inside the handler. The compiler
 * emits `push rbp` as prologue, so after that RSP is shifted by 8.
 * Correct entry RSP (≡ 8 mod 16) → after push rbp: ≡ 0 mod 16.
 * Buggy entry RSP  (≡ 0 mod 16) → after push rbp: ≡ 8 mod 16.
 */

static volatile uint64_t handler_rsp = 0;

static void alignment_handler(int sig) {
    (void)sig;
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    handler_rsp = rsp;
}

static void test_sig_alignment(void) {
    puts("\n[SIG-BUG2: RSP Alignment]\n");

    handler_rsp = 0;

    struct ksigaction_b sa = {
        .handler  = (void *)alignment_handler,
        .flags    = SA_RESTORER,
        .restorer = (void *)sig_restorer_b,
        .mask     = 0,
    };
    sc4(SYS_RT_SIGACTION, SIGUSR1, (long)&sa, 0, 8);

    sc2(SYS_KILL, sc0(SYS_GETPID), SIGUSR1);

    uint64_t mod = handler_rsp & 0xF;
    puts("  handler RSP=0x"); put_hex(handler_rsp); puts("\n");
    puts("  RSP mod 16=");    put_int((long)mod); puts("\n");

    /* After push rbp, RSP should be ≡ 0 (mod 16).
     * This means function-entry RSP was ≡ 8 (mod 16). */
    check_val("handler RSP mod 16 after push rbp", (long)mod, 0);
}
TEST("sig-bug2-alignment", test_sig_alignment);

/* ── BUG-SIG3: sig_blocked is process-level, not per-thread ──
 *
 * Sequence:
 * 1. SIGUSR1 unblocked, spawn thread B (inherits unblocked mask)
 * 2. Thread A blocks SIGUSR1
 * 3. Send SIGUSR1 to thread B via tgkill
 *
 * Correct (per-thread): B still has SIGUSR1 unblocked → delivered.
 * Bug (process-level):  A's block also affects B → not delivered.
 */

static volatile int b_received = 0;
static volatile int b_ready = 0;
static volatile int b_done = 0;

static void b_handler(int sig) {
    (void)sig;
    __sync_fetch_and_add((int *)&b_received, 1);
}

static void b_thread_fn(void) {
    __sync_fetch_and_add((int *)&b_ready, 1);
    while (!b_done)
        __asm__ volatile("pause");
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static void test_sig_per_thread_mask(void) {
    puts("\n[SIG-BUG3: per-thread sig_blocked]\n");

    b_received = 0;
    b_ready = 0;
    b_done = 0;

    /* Install handler */
    struct ksigaction_b sa = {
        .handler  = (void *)b_handler,
        .flags    = SA_RESTORER,
        .restorer = (void *)sig_restorer_b,
        .mask     = 0,
    };
    sc4(SYS_RT_SIGACTION, SIGUSR1, (long)&sa, 0, 8);

    /* Ensure SIGUSR1 unblocked before clone */
    uint64_t mask = (1ULL << SIGUSR1);
    sc4(SYS_RT_SIGPROCMASK, 1 /* SIG_UNBLOCK */, (long)&mask, 0, 8);

    /* Spawn thread B — inherits unblocked mask */
    long stack = sc6(SYS_MMAP, 0, 65536, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap thread B stack", stack > 0);
    if (stack <= 0) return;

    long tid_b = sc5(SYS_CLONE,
                     (long)(CLONE_VM | CLONE_FS | CLONE_FILES |
                            CLONE_SIGHAND | CLONE_THREAD),
                     stack + 65536, 0, 0, 0);
    if (tid_b == 0) {
        b_thread_fn();
        __builtin_unreachable();
    }
    check("clone thread B", tid_b > 0);

    /* Wait for B */
    for (volatile int i = 0; i < 10000000 && !b_ready; i++)
        __asm__ volatile("pause");
    check("thread B ready", b_ready > 0);

    /* Thread A: block SIGUSR1 — must NOT affect thread B */
    sc4(SYS_RT_SIGPROCMASK, 0 /* SIG_BLOCK */, (long)&mask, 0, 8);

    /* Send SIGUSR1 to thread B */
    sc3(SYS_TGKILL, sc0(SYS_GETPID), tid_b, SIGUSR1);

    /* Give B time to handle */
    for (volatile int i = 0; i < 10000000; i++)
        __asm__ volatile("pause");

    puts("  b_received="); put_int(b_received); puts("\n");
    check_val("thread B received SIGUSR1", b_received, 1);

    /* Cleanup */
    sc4(SYS_RT_SIGPROCMASK, 1 /* SIG_UNBLOCK */, (long)&mask, 0, 8);
    b_done = 1;

    /* Let B exit */
    for (volatile int i = 0; i < 1000000; i++)
        __asm__ volatile("pause");
}
TEST("sig-bug3-per-thread-mask", test_sig_per_thread_mask);
