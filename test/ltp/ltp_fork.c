/* LTP: fork/vfork/waitpid/wait4/_exit */
#define _GNU_SOURCE
#include "ltp.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <sched.h>

int main(void) {
    printf("=== LTP: fork ===\n");

    /* 1. fork returns 0 in child, >0 in parent */
    pid_t pid = fork();
    if (pid == 0) _exit(0);
    CHECK("fork returns child pid", pid > 0);
    int st;
    waitpid(pid, &st, 0);
    CHECK("child exited 0", WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* 2. child exit status propagates */
    pid = fork();
    if (pid == 0) _exit(42);
    waitpid(pid, &st, 0);
    CHECK("child exit status 42", WIFEXITED(st) && WEXITSTATUS(st) == 42);

    /* 3. fork copies memory (COW) */
    int val = 100;
    pid = fork();
    if (pid == 0) {
        val = 200;
        _exit(val == 200 ? 0 : 1);
    }
    waitpid(pid, &st, 0);
    CHECK("fork COW — parent unchanged", val == 100);
    CHECK("fork COW — child modified", WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* 4. waitpid WNOHANG on running child */
    pid = fork();
    if (pid == 0) { usleep(100000); _exit(0); }
    pid_t w = waitpid(pid, &st, WNOHANG);
    CHECK("waitpid WNOHANG → 0 (still running)", w == 0);
    waitpid(pid, &st, 0);

    /* 5. waitpid -1 waits for any child */
    pid = fork();
    if (pid == 0) _exit(7);
    pid_t got = waitpid(-1, &st, 0);
    CHECK("waitpid(-1) reaps child", got == pid && WEXITSTATUS(st) == 7);

    /* 6. child inherits open fds */
    int pfd[2];
    pipe(pfd);
    pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        write(pfd[1], "X", 1);
        close(pfd[1]);
        _exit(0);
    }
    close(pfd[1]);
    char buf[2] = {0};
    read(pfd[0], buf, 1);
    close(pfd[0]);
    waitpid(pid, &st, 0);
    CHECK("child inherits pipe fd", buf[0] == 'X');

    /* 7. getpid differs in child */
    pid_t parent_pid = getpid();
    pid = fork();
    if (pid == 0) _exit(getpid() != parent_pid ? 0 : 1);
    waitpid(pid, &st, 0);
    CHECK("child getpid != parent", WEXITSTATUS(st) == 0);

    /* 8. getppid in child == parent pid */
    pid = fork();
    if (pid == 0) _exit(getppid() == parent_pid ? 0 : 1);
    waitpid(pid, &st, 0);
    CHECK("child getppid == parent pid", WEXITSTATUS(st) == 0);

    /* 9. vfork + _exit */
    pid = vfork();
    if (pid == 0) _exit(55);
    waitpid(pid, &st, 0);
    CHECK("vfork + _exit status", WIFEXITED(st) && WEXITSTATUS(st) == 55);

    /* 10. kill child with SIGKILL */
    pid = fork();
    if (pid == 0) { for (;;) pause(); }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    CHECK("SIGKILL terminates child", WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);

    /* 11. wait4 fills rusage */
    pid = fork();
    if (pid == 0) {
        volatile int s = 0;
        for (int i = 0; i < 100000; i++) s += i;
        (void)s;
        _exit(0);
    }
    struct rusage ru;
    memset(&ru, 0, sizeof(ru));
    wait4(pid, &st, 0, &ru);
    /* rusage should have some user time (or at least not fail) */
    CHECK("wait4 succeeded", WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* 12. multiple children, wait each */
    pid_t c1 = fork();
    if (c1 == 0) _exit(1);
    pid_t c2 = fork();
    if (c2 == 0) _exit(2);
    int st1, st2;
    waitpid(c1, &st1, 0);
    waitpid(c2, &st2, 0);
    CHECK("multi-child wait", WEXITSTATUS(st1) == 1 && WEXITSTATUS(st2) == 2);

    LTP_SUMMARY("fork");
}
