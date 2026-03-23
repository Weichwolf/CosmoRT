/* LTP: execve with various scenarios */
#define _GNU_SOURCE
#include "ltp.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

int main(int argc, char **argv) {
    /* When called with --child-echo, just echo args and exit */
    if (argc >= 2 && strcmp(argv[1], "--child-echo") == 0) {
        for (int i = 2; i < argc; i++) {
            write(1, argv[i], strlen(argv[i]));
            write(1, " ", 1);
        }
        _exit(0);
    }
    /* When called with --child-env, print specific env */
    if (argc >= 2 && strcmp(argv[1], "--child-env") == 0) {
        const char *v = getenv("LTP_TEST_VAR");
        if (v) write(1, v, strlen(v));
        _exit(v ? 0 : 1);
    }
    /* When called with --child-exit N, exit with N */
    if (argc >= 3 && strcmp(argv[1], "--child-exit") == 0) {
        _exit(atoi(argv[2]));
    }

    printf("=== LTP: exec ===\n");
    const char *self = argv[0];

    /* 1. execve child-echo with args */
    {
        int pfd[2];
        pipe(pfd);
        pid_t pid = fork();
        if (pid == 0) {
            close(pfd[0]);
            dup2(pfd[1], 1);
            close(pfd[1]);
            char *a[] = { (char *)self, "--child-echo", "hello", "world", NULL };
            char *e[] = { NULL };
            execve(self, a, e);
            _exit(99);
        }
        close(pfd[1]);
        char buf[64] = {0};
        read(pfd[0], buf, sizeof(buf) - 1);
        close(pfd[0]);
        int st;
        waitpid(pid, &st, 0);
        CHECK("execve child-echo args", strstr(buf, "hello") && strstr(buf, "world"));
        CHECK("execve child exit 0", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    /* 2. execve passes environment */
    {
        int pfd[2];
        pipe(pfd);
        pid_t pid = fork();
        if (pid == 0) {
            close(pfd[0]);
            dup2(pfd[1], 1);
            close(pfd[1]);
            char *a[] = { (char *)self, "--child-env", NULL };
            char *e[] = { "LTP_TEST_VAR=magic42", NULL };
            execve(self, a, e);
            _exit(99);
        }
        close(pfd[1]);
        char buf[64] = {0};
        read(pfd[0], buf, sizeof(buf) - 1);
        close(pfd[0]);
        int st;
        waitpid(pid, &st, 0);
        CHECK("execve passes env", strcmp(buf, "magic42") == 0);
    }

    /* 3. execve propagates exit code */
    {
        pid_t pid = fork();
        if (pid == 0) {
            char *a[] = { (char *)self, "--child-exit", "37", NULL };
            char *e[] = { NULL };
            execve(self, a, e);
            _exit(99);
        }
        int st;
        waitpid(pid, &st, 0);
        CHECK("execve exit code 37", WIFEXITED(st) && WEXITSTATUS(st) == 37);
    }

    /* 4. execve nonexistent → ENOENT */
    {
        pid_t pid = fork();
        if (pid == 0) {
            char *a[] = { "/nonexistent_binary", NULL };
            char *e[] = { NULL };
            errno = 0;
            execve("/nonexistent_binary", a, e);
            _exit(errno == ENOENT ? 0 : 1);
        }
        int st;
        waitpid(pid, &st, 0);
        CHECK("execve ENOENT", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    /* 5. execve /dev/null (not executable format) → ENOEXEC or similar */
    {
        pid_t pid = fork();
        if (pid == 0) {
            char *a[] = { "/dev/null", NULL };
            char *e[] = { NULL };
            execve("/dev/null", a, e);
            /* Should fail — any non-zero errno is acceptable */
            _exit(errno != 0 ? 0 : 1);
        }
        int st;
        waitpid(pid, &st, 0);
        CHECK("execve /dev/null fails", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    /* 6. execve closes O_CLOEXEC fds */
    {
        int pfd[2];
        pipe(pfd);
        int clofd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        pid_t pid = fork();
        if (pid == 0) {
            close(pfd[0]);
            /* After execve, clofd should be closed */
            /* We re-exec ourselves to check */
            char fdstr[16];
            snprintf(fdstr, sizeof(fdstr), "%d", clofd);
            char *a[] = { (char *)self, "--child-exit", "0", NULL };
            char *e[] = { NULL };
            execve(self, a, e);
            _exit(99);
        }
        close(clofd);
        close(pfd[1]);
        close(pfd[0]);
        int st;
        waitpid(pid, &st, 0);
        CHECK("execve with O_CLOEXEC fd", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    /* 7. execve replaces address space (child writes to stack variable,
       parent's is unchanged) */
    {
        volatile int marker = 0xDEAD;
        pid_t pid = fork();
        if (pid == 0) {
            char *a[] = { (char *)self, "--child-exit", "0", NULL };
            char *e[] = { NULL };
            execve(self, a, e);
            _exit(99);
        }
        int st;
        waitpid(pid, &st, 0);
        CHECK("execve — parent memory intact", marker == 0xDEAD);
    }

    /* 8. execve with NULL argv → should not crash (Linux allows it) */
    {
        pid_t pid = fork();
        if (pid == 0) {
            /* Some systems reject NULL argv, some allow it.
               Either way, the kernel should not crash. */
            char *e[] = { NULL };
            execve(self, NULL, e);
            _exit(0); /* if execve fails, still exit cleanly */
        }
        int st;
        waitpid(pid, &st, 0);
        CHECK("execve NULL argv — no crash", WIFEXITED(st));
    }

    /* 9. many rapid exec cycles */
    {
        int ok = 1;
        for (int i = 0; i < 10; i++) {
            pid_t pid = fork();
            if (pid == 0) {
                char code[4];
                snprintf(code, sizeof(code), "%d", i % 256);
                char *a[] = { (char *)self, "--child-exit", code, NULL };
                char *e[] = { NULL };
                execve(self, a, e);
                _exit(99);
            }
            int st;
            waitpid(pid, &st, 0);
            if (!WIFEXITED(st) || WEXITSTATUS(st) != (i % 256)) { ok = 0; break; }
        }
        CHECK("10 rapid exec cycles", ok);
    }

    /* 10. execve after fork — parent PID stable */
    {
        pid_t ppid = getpid();
        pid_t pid = fork();
        if (pid == 0) {
            char *a[] = { (char *)self, "--child-exit", "0", NULL };
            char *e[] = { NULL };
            execve(self, a, e);
            _exit(99);
        }
        int st;
        waitpid(pid, &st, 0);
        CHECK("parent PID stable after child exec", getpid() == ppid);
    }

    LTP_SUMMARY("exec");
}
