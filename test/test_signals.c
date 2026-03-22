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

static volatile int sig_received = 0;
static volatile int sig_value = 0;

__attribute__((used)) static void test_handler(int sig) {
    sig_received = 1;
    sig_value = sig;
}

__attribute__((used)) static void test_handler_siginfo(int sig, void *info, void *uctx) {
    (void)info; (void)uctx;
    sig_received = 1;
    sig_value = sig;
}

void test_signals(void) {
    puts("\n[Signals]\n");

    /* Test 1: SIG_IGN via rt_sigaction */
    struct ksigaction sa, old;
    sa.handler = (void *)1; /* SIG_IGN */
    sa.flags = 0;
    sa.restorer = 0;
    sa.mask = 0;
    long r = sc4(SYS_rt_sigaction, SIGUSR1, (long)&sa, (long)&old, 8);
    check_val("rt_sigaction SIG_IGN", r, 0);

    /* Send SIGUSR1 to self — should not crash (SIG_IGN) */
    r = sc2(SYS_kill, sc0(SYS_getpid), SIGUSR1);
    check_val("kill(self, SIGUSR1) SIG_IGN", r, 0);

    /* Test 2: User signal handler */
    sig_received = 0;
    sig_value = 0;
    sa.handler = (void *)test_handler;
    sa.flags = SA_RESTORER;
    sa.restorer = (void *)sig_restorer;
    sa.mask = 0;
    r = sc4(SYS_rt_sigaction, SIGUSR1, (long)&sa, 0, 8);
    check_val("rt_sigaction user handler", r, 0);

    /* Send SIGUSR1 to self — handler should run */
    r = sc2(SYS_kill, sc0(SYS_getpid), SIGUSR1);
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
    sc4(SYS_rt_sigaction, SIGUSR2, (long)&sa, 0, 8);
    sc2(SYS_kill, sc0(SYS_getpid), SIGUSR2);
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
    sc4(SYS_rt_sigaction, SIGUSR1, (long)&sa, 0, 8);

    uint64_t block_mask = (1ULL << SIGUSR1);
    uint64_t old_mask = 0;
    sc4(SYS_rt_sigprocmask, 0 /* SIG_BLOCK */, (long)&block_mask, (long)&old_mask, 8);

    /* Send while blocked — should be queued */
    sc2(SYS_kill, sc0(SYS_getpid), SIGUSR1);
    check_val("blocked sig not delivered", sig_received, 0);

    /* Unblock — should deliver now */
    sc4(SYS_rt_sigprocmask, 1 /* SIG_UNBLOCK */, (long)&block_mask, 0, 8);
    check_val("unblocked sig delivered", sig_received, 1);
    check_val("unblocked sig value", sig_value, SIGUSR1);

    /* Test 5: oldact returned correctly */
    sa.handler = (void *)test_handler;
    sa.flags = SA_RESTORER;
    sa.restorer = (void *)sig_restorer;
    sa.mask = 0;
    sc4(SYS_rt_sigaction, SIGUSR1, (long)&sa, 0, 8);
    struct ksigaction got;
    sc4(SYS_rt_sigaction, SIGUSR1, 0, (long)&got, 8);
    check("oldact handler matches", got.handler == (void *)test_handler);
    check("oldact restorer matches", got.restorer == (void *)sig_restorer);
}
