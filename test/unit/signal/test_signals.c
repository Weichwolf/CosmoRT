#include "ktest.h"

struct ksigaction {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

/* Minimal restorer trampoline: mov rax, 15; syscall */
__attribute__((naked)) static void sig_restorer(void) {
    __asm__ volatile(
        "mov $15, %%rax\n"
        "syscall\n"
        ::: "memory"
    );
}

/* Linux wait status macros */
#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)

/* Minimal siginfo_t layout (first 16 bytes matter) */
struct test_siginfo {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t _pad0;
};

static volatile int sig_received = 0;
static volatile int sig_value = 0;
static volatile int siginfo_signo = 0;
static volatile void *siginfo_ptr = 0;

__attribute__((used)) static void test_handler(int sig) {
    sig_received = 1;
    sig_value = sig;
}

__attribute__((used)) static void test_handler_siginfo(int sig, void *info, void *uctx) {
    (void)uctx;
    sig_received = 1;
    sig_value = sig;
    siginfo_ptr = info;
    if (info) {
        struct test_siginfo *si = (struct test_siginfo *)info;
        siginfo_signo = si->si_signo;
    }
}

static void test_signals(void) {
    puts("\n[Signals]\n");

    /* Test 1: SIG_IGN via rt_sigaction */
    struct ksigaction sa, old;
    sa.handler = (void *)1; /* SIG_IGN */
    sa.flags = 0;
    sa.restorer = 0;
    sa.mask = 0;
    long r = sc4(SYS_RT_SIGACTION, SIGUSR1, (long)&sa, (long)&old, 8);
    check_val("rt_sigaction SIG_IGN", r, 0);

    /* Send SIGUSR1 to self — should not crash (SIG_IGN) */
    r = sc2(SYS_KILL, sc0(SYS_GETPID), SIGUSR1);
    check_val("kill(self, SIGUSR1) SIG_IGN", r, 0);

    /* Test 2: User signal handler */
    sig_received = 0;
    sig_value = 0;
    sa.handler = (void *)test_handler;
    sa.flags = SA_RESTORER;
    sa.restorer = (void *)sig_restorer;
    sa.mask = 0;
    r = sc4(SYS_RT_SIGACTION, SIGUSR1, (long)&sa, 0, 8);
    check_val("rt_sigaction user handler", r, 0);

    /* Send SIGUSR1 to self — handler should run */
    r = sc2(SYS_KILL, sc0(SYS_GETPID), SIGUSR1);
    check_val("kill(self, SIGUSR1) handler", r, 0);
    check_val("handler received sig", sig_received, 1);
    check_val("handler got SIGUSR1", sig_value, SIGUSR1);

    /* Test 3: Second signal (SIGUSR2) with different handler */
    sig_received = 0;
    sig_value = 0;
    sa.handler = (void *)test_handler;
    sa.flags = SA_RESTORER;
    sa.restorer = (void *)sig_restorer;
    sa.mask = 0;
    sc4(SYS_RT_SIGACTION, SIGUSR2, (long)&sa, 0, 8);
    sc2(SYS_KILL, sc0(SYS_GETPID), SIGUSR2);
    check_val("SIGUSR2 handler ran", sig_received, 1);
    check_val("SIGUSR2 value", sig_value, SIGUSR2);

    /* Test 4: rt_sigprocmask — block SIGUSR1, send it, unblock, check delivery */
    sig_received = 0;
    sig_value = 0;
    /* Re-register handler (signal was auto-blocked during delivery) */
    sa.handler = (void *)test_handler;
    sa.flags = SA_RESTORER;
    sa.restorer = (void *)sig_restorer;
    sa.mask = 0;
    sc4(SYS_RT_SIGACTION, SIGUSR1, (long)&sa, 0, 8);

    uint64_t block_mask = (1ULL << SIGUSR1);
    uint64_t old_mask = 0;
    sc4(SYS_RT_SIGPROCMASK, 0 /* SIG_BLOCK */, (long)&block_mask, (long)&old_mask, 8);

    /* Send while blocked — should be queued */
    sc2(SYS_KILL, sc0(SYS_GETPID), SIGUSR1);
    check_val("blocked sig not delivered", sig_received, 0);

    /* Unblock — should deliver now */
    sc4(SYS_RT_SIGPROCMASK, 1 /* SIG_UNBLOCK */, (long)&block_mask, 0, 8);
    check_val("unblocked sig delivered", sig_received, 1);
    check_val("unblocked sig value", sig_value, SIGUSR1);

    /* Test 5: oldact returned correctly */
    sa.handler = (void *)test_handler;
    sa.flags = SA_RESTORER;
    sa.restorer = (void *)sig_restorer;
    sa.mask = 0;
    sc4(SYS_RT_SIGACTION, SIGUSR1, (long)&sa, 0, 8);
    struct ksigaction got;
    sc4(SYS_RT_SIGACTION, SIGUSR1, 0, (long)&got, 8);
    check("oldact handler matches", got.handler == (void *)test_handler);
    check("oldact restorer matches", got.restorer == (void *)sig_restorer);

    /* ── RT signals (32-63) ── */

    /* Test 6: rt_sigaction on signal 32 (SIGRTMIN) */
    sig_received = 0;
    sig_value = 0;
    sa.handler = (void *)test_handler;
    sa.flags = SA_RESTORER;
    sa.restorer = (void *)sig_restorer;
    sa.mask = 0;
    r = sc4(SYS_RT_SIGACTION, 32, (long)&sa, 0, 8);
    check_val("rt_sigaction sig 32", r, 0);

    /* Send signal 32 to self — handler should run */
    r = sc2(SYS_KILL, sc0(SYS_GETPID), 32);
    check_val("kill(self, 32)", r, 0);
    check_val("sig 32 handler ran", sig_received, 1);
    check_val("sig 32 value", sig_value, 32);

    /* Test 7: RT signal 63 (SIGRTMAX) */
    sig_received = 0;
    sig_value = 0;
    sa.handler = (void *)test_handler;
    sa.flags = SA_RESTORER;
    sa.restorer = (void *)sig_restorer;
    sa.mask = 0;
    sc4(SYS_RT_SIGACTION, 63, (long)&sa, 0, 8);
    sc2(SYS_KILL, sc0(SYS_GETPID), 63);
    check_val("sig 63 handler ran", sig_received, 1);
    check_val("sig 63 value", sig_value, 63);

    /* Test 8: tgkill — thread-directed signal */
    sig_received = 0;
    sig_value = 0;
    sa.handler = (void *)test_handler;
    sa.flags = SA_RESTORER;
    sa.restorer = (void *)sig_restorer;
    sa.mask = 0;
    sc4(SYS_RT_SIGACTION, SIGUSR1, (long)&sa, 0, 8);

    long my_pid = sc0(SYS_GETPID);
    long my_tid = sc0(SYS_GETTID);
    r = sc3(SYS_TGKILL, my_pid, my_tid, SIGUSR1);
    check_val("tgkill(self) returns 0", r, 0);
    check_val("tgkill handler ran", sig_received, 1);
    check_val("tgkill handler got SIGUSR1", sig_value, SIGUSR1);

    /* Test 9: tgkill with bad tgid → -EINVAL */
    r = sc3(SYS_TGKILL, -1, my_tid, SIGUSR1);
    check("tgkill bad tgid → error", r < 0);

    /* Test 10: tgkill sig 0 (permission check only) */
    r = sc3(SYS_TGKILL, my_pid, my_tid, 0);
    check_val("tgkill sig 0", r, 0);

    /* ── SA_SIGINFO ── */

    /* Test 11: SA_SIGINFO handler receives siginfo_t with correct si_signo */
    sig_received = 0;
    sig_value = 0;
    siginfo_signo = 0;
    siginfo_ptr = 0;
    sa.handler = (void *)test_handler_siginfo;
    sa.flags = SA_RESTORER | SA_SIGINFO;
    sa.restorer = (void *)sig_restorer;
    sa.mask = 0;
    sc4(SYS_RT_SIGACTION, SIGUSR1, (long)&sa, 0, 8);
    sc2(SYS_KILL, sc0(SYS_GETPID), SIGUSR1);
    check_val("SA_SIGINFO handler ran", sig_received, 1);
    check_val("SA_SIGINFO sig value", sig_value, SIGUSR1);
    check("SA_SIGINFO info ptr non-null", siginfo_ptr != 0);
    check_val("SA_SIGINFO si_signo", siginfo_signo, SIGUSR1);

    /* ── Signal default actions ── */

    /* Test 12: SIGHUP with SIG_DFL → child terminated by signal.
     * Child resets handler to SIG_DFL (may be SIG_IGN from earlier tests),
     * then sends itself SIGHUP. */
    {
        long pid = sc0(SYS_FORK);
        if (pid == 0) {
            /* Reset to SIG_DFL */
            struct ksigaction dfl;
            dfl.handler = (void *)0; /* SIG_DFL */
            dfl.flags = 0; dfl.restorer = 0; dfl.mask = 0;
            sc4(SYS_RT_SIGACTION, SIGHUP, (long)&dfl, 0, 8);
            sc2(SYS_KILL, sc0(SYS_GETPID), SIGHUP);
            sc1(SYS_EXIT_GROUP, 99); /* should not reach */
            __builtin_unreachable();
        }
        check("fork SIGHUP ok", pid > 0);
        int wstatus = 0;
        sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
        check("SIGHUP WIFSIGNALED", WIFSIGNALED(wstatus));
        check_val("SIGHUP WTERMSIG", WTERMSIG(wstatus), SIGHUP);
    }

    /* Test 13: SIGALRM with SIG_DFL → child terminated by signal */
    {
        long pid = sc0(SYS_FORK);
        if (pid == 0) {
            struct ksigaction dfl;
            dfl.handler = (void *)0;
            dfl.flags = 0; dfl.restorer = 0; dfl.mask = 0;
            sc4(SYS_RT_SIGACTION, SIGALRM, (long)&dfl, 0, 8);
            sc2(SYS_KILL, sc0(SYS_GETPID), SIGALRM);
            sc1(SYS_EXIT_GROUP, 99);
            __builtin_unreachable();
        }
        check("fork SIGALRM ok", pid > 0);
        int wstatus = 0;
        sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
        check("SIGALRM WIFSIGNALED", WIFSIGNALED(wstatus));
        check_val("SIGALRM WTERMSIG", WTERMSIG(wstatus), SIGALRM);
    }
}

TEST("signals", test_signals);
