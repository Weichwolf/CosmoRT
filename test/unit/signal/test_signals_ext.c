/* Extended signal tests: SIGPIPE, SIGALRM, SIGCHLD */

#include "ktest.h"

#define SIGPIPE  13
#define SIGALRM  14
#define SIGCHLD  17

#define SYS_ALARM 37

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)

struct ksigaction {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

__attribute__((naked)) static void sig_restorer(void) {
    __asm__ volatile(
        "mov $15, %%rax\n"
        "syscall\n"
        ::: "memory"
    );
}

static volatile int got_sigpipe = 0;
static volatile int got_sigalrm = 0;
static volatile int got_sigchld = 0;

__attribute__((used)) static void sigpipe_handler(int sig) {
    (void)sig;
    got_sigpipe = 1;
}

__attribute__((used)) static void sigalrm_handler(int sig) {
    (void)sig;
    got_sigalrm = 1;
}

__attribute__((used)) static void sigchld_handler(int sig) {
    (void)sig;
    got_sigchld = 1;
}

static void msleep(int ms) {
    struct { long sec; long nsec; } ts = { ms / 1000, (ms % 1000) * 1000000L };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
}

static void test_signals_ext(void) {
    puts("\n[Signals Extended]\n");

    /* ── SIGPIPE: write on closed pipe ── */
    {
        got_sigpipe = 0;
        struct ksigaction sa;
        sa.handler = (void *)sigpipe_handler;
        sa.flags = SA_RESTORER;
        sa.restorer = (void *)sig_restorer;
        sa.mask = 0;
        sc4(SYS_RT_SIGACTION, SIGPIPE, (long)&sa, 0, 8);

        int fds[2];
        long r = sc2(SYS_PIPE, (long)fds, 0);
        check_val("pipe for SIGPIPE", r, 0);

        /* Close read end */
        sc1(SYS_CLOSE, fds[0]);

        /* Write to broken pipe */
        char buf = 'x';
        r = sc3(SYS_WRITE, fds[1], (long)&buf, 1);
        check_val("write broken pipe -> -EPIPE", r, -EPIPE);
        check_val("SIGPIPE received", got_sigpipe, 1);

        sc1(SYS_CLOSE, fds[1]);
    }

    /* ── SIGPIPE with SIG_IGN: no signal, still -EPIPE ── */
    {
        got_sigpipe = 0;
        struct ksigaction sa;
        sa.handler = SIG_IGN;
        sa.flags = 0;
        sa.restorer = 0;
        sa.mask = 0;
        sc4(SYS_RT_SIGACTION, SIGPIPE, (long)&sa, 0, 8);

        int fds[2];
        sc2(SYS_PIPE, (long)fds, 0);
        sc1(SYS_CLOSE, fds[0]);

        char buf = 'x';
        long r = sc3(SYS_WRITE, fds[1], (long)&buf, 1);
        check_val("write broken pipe SIG_IGN -> -EPIPE", r, -EPIPE);
        check_val("SIGPIPE not received (SIG_IGN)", got_sigpipe, 0);

        sc1(SYS_CLOSE, fds[1]);
    }

    /* ── SIGALRM: alarm(1) fires after ~1 second ── */
    {
        got_sigalrm = 0;
        struct ksigaction sa;
        sa.handler = (void *)sigalrm_handler;
        sa.flags = SA_RESTORER;
        sa.restorer = (void *)sig_restorer;
        sa.mask = 0;
        sc4(SYS_RT_SIGACTION, SIGALRM, (long)&sa, 0, 8);

        sc1(SYS_ALARM, 0); /* cancel test-runner watchdog */
        long old = sc1(SYS_ALARM, 1);
        check_val("alarm(1) old=0", old, 0);

        /* Wait ~1.1 seconds for alarm to fire */
        msleep(1100);
        check_val("SIGALRM received", got_sigalrm, 1);
    }

    /* ── SIGALRM: alarm(0) cancels ── */
    {
        got_sigalrm = 0;
        struct ksigaction sa;
        sa.handler = (void *)sigalrm_handler;
        sa.flags = SA_RESTORER;
        sa.restorer = (void *)sig_restorer;
        sa.mask = 0;
        sc4(SYS_RT_SIGACTION, SIGALRM, (long)&sa, 0, 8);

        sc1(SYS_ALARM, 2);
        long rem = sc1(SYS_ALARM, 0);
        check("alarm(0) returns remaining > 0", rem > 0);

        msleep(2200);
        check_val("SIGALRM not received (cancelled)", got_sigalrm, 0);
    }

    /* ── SIGCHLD: fork + child exit → parent gets SIGCHLD ── */
    {
        got_sigchld = 0;
        struct ksigaction sa;
        sa.handler = (void *)sigchld_handler;
        sa.flags = SA_RESTORER;
        sa.restorer = (void *)sig_restorer;
        sa.mask = 0;
        sc4(SYS_RT_SIGACTION, SIGCHLD, (long)&sa, 0, 8);

        long pid = sc0(SYS_FORK);
        if (pid == 0) {
            sc1(SYS_EXIT_GROUP, 42);
            __builtin_unreachable();
        }
        check("fork for SIGCHLD ok", pid > 0);

        int wstatus = 0;
        sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
        check("child exited normally", WIFEXITED(wstatus));
        check_val("child exit code", WEXITSTATUS(wstatus), 42);
        check_val("SIGCHLD received", got_sigchld, 1);
    }
}

TEST("signals_ext", test_signals_ext);
