/* LTP: sigaction/kill/sigprocmask/sigsuspend/raise */
#define _GNU_SOURCE
#include "ltp.h"
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

static volatile sig_atomic_t _sig_received;
static volatile sig_atomic_t _sig_info_val;

static void handler_count(int sig) {
    (void)sig;
    _sig_received++;
}

static void handler_info(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)ctx;
    _sig_info_val = si->si_value.sival_int;
}

int main(void) {
    printf("=== LTP: signal ===\n");

    /* 1. sigaction installs handler, raise delivers */
    struct sigaction sa = { .sa_handler = handler_count };
    sigemptyset(&sa.sa_mask);
    int r = sigaction(SIGUSR1, &sa, NULL);
    CHECK("sigaction install", r == 0);
    _sig_received = 0;
    raise(SIGUSR1);
    CHECK("raise SIGUSR1 delivered", _sig_received == 1);

    /* 2. SIG_IGN ignores signal */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGUSR1, &sa, NULL);
    _sig_received = 0;
    raise(SIGUSR1);
    CHECK("SIG_IGN ignores", _sig_received == 0);

    /* 3. sigprocmask blocks signal */
    sa.sa_handler = handler_count;
    sigaction(SIGUSR1, &sa, NULL);
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block, &old);
    _sig_received = 0;
    raise(SIGUSR1);
    CHECK("blocked signal not delivered", _sig_received == 0);

    /* 4. sigpending shows blocked pending signal */
    sigset_t pend;
    sigpending(&pend);
    CHECK("sigpending shows SIGUSR1", sigismember(&pend, SIGUSR1));

    /* 5. unblock delivers pending signal */
    sigprocmask(SIG_UNBLOCK, &block, NULL);
    CHECK("unblock delivers pending", _sig_received == 1);

    /* 6. kill sends signal to child */
    _sig_received = 0;
    sa.sa_handler = handler_count;
    sigaction(SIGUSR2, &sa, NULL);
    pid_t pid = fork();
    if (pid == 0) {
        struct sigaction csa = { .sa_handler = handler_count };
        sigemptyset(&csa.sa_mask);
        sigaction(SIGUSR2, &csa, NULL);
        _sig_received = 0;
        pause(); /* wait for signal */
        _exit(_sig_received == 1 ? 0 : 1);
    }
    usleep(10000);
    kill(pid, SIGUSR2);
    int st;
    waitpid(pid, &st, 0);
    CHECK("kill to child", WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* 7. sigsuspend atomically unblocks and waits */
    sa.sa_handler = handler_count;
    sigaction(SIGALRM, &sa, NULL);
    _sig_received = 0;
    alarm(1); /* SIGALRM in 1 second — but we'll use a faster method */
    /* Use fork to send signal quickly */
    pid = fork();
    if (pid == 0) {
        usleep(10000);
        kill(getppid(), SIGUSR1);
        _exit(0);
    }
    sigset_t empty;
    sigemptyset(&empty);
    sa.sa_handler = handler_count;
    sigaction(SIGUSR1, &sa, NULL);
    _sig_received = 0;
    sigsuspend(&empty); /* returns when signal delivered */
    CHECK("sigsuspend woke on signal", _sig_received == 1);
    waitpid(pid, NULL, 0);
    alarm(0);

    /* 8. SA_SIGINFO provides siginfo */
    struct sigaction sa2 = {
        .sa_sigaction = handler_info,
        .sa_flags = SA_SIGINFO,
    };
    sigemptyset(&sa2.sa_mask);
    sigaction(SIGUSR1, &sa2, NULL);
    _sig_info_val = 0;
    union sigval sv = { .sival_int = 12345 };
    sigqueue(getpid(), SIGUSR1, sv);
    CHECK("SA_SIGINFO si_value", _sig_info_val == 12345);

    /* 9. signal to self with kill */
    sa.sa_handler = handler_count;
    sigaction(SIGUSR1, &sa, NULL);
    _sig_received = 0;
    kill(getpid(), SIGUSR1);
    CHECK("kill(getpid, SIGUSR1)", _sig_received == 1);

    /* 10. multiple signals coalesce (standard signals) */
    sigprocmask(SIG_BLOCK, &block, NULL); /* block SIGUSR1 */
    _sig_received = 0;
    for (int i = 0; i < 5; i++) raise(SIGUSR1);
    sigprocmask(SIG_UNBLOCK, &block, NULL);
    /* Standard signals are not queued — only 1 delivery guaranteed */
    CHECK("standard signals coalesce (>=1)", _sig_received >= 1);

    /* 11. SIGCHLD on child exit */
    sa.sa_handler = handler_count;
    sigaction(SIGCHLD, &sa, NULL);
    _sig_received = 0;
    pid = fork();
    if (pid == 0) _exit(0);
    waitpid(pid, NULL, 0);
    /* SIGCHLD may or may not be delivered by the time waitpid returns,
       but the handler should eventually fire */
    usleep(1000);
    CHECK("SIGCHLD on child exit", _sig_received >= 1);

    /* 12. SIG_DFL restore */
    sa.sa_handler = SIG_DFL;
    r = sigaction(SIGUSR1, &sa, NULL);
    CHECK("sigaction restore SIG_DFL", r == 0);

    LTP_SUMMARY("signal");
}
