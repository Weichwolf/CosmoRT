/* LTP: pipe/pipe2/blocking read-write/close */
#define _GNU_SOURCE
#include "ltp.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>

int main(void) {
    printf("=== LTP: pipe ===\n");
    int pfd[2];

    /* 1. pipe creates two fds */
    int r = pipe(pfd);
    CHECK("pipe() returns 0", r == 0);
    CHECK("pipe fds valid", pfd[0] >= 0 && pfd[1] >= 0 && pfd[0] != pfd[1]);
    close(pfd[0]); close(pfd[1]);

    /* 2. write then read */
    pipe(pfd);
    write(pfd[1], "abc", 3);
    char buf[8] = {0};
    ssize_t n = read(pfd[0], buf, sizeof(buf));
    CHECK("pipe write+read", n == 3 && memcmp(buf, "abc", 3) == 0);
    close(pfd[0]); close(pfd[1]);

    /* 3. read after writer closes → EOF */
    pipe(pfd);
    write(pfd[1], "x", 1);
    close(pfd[1]);
    char b2[4] = {0};
    read(pfd[0], b2, 1); /* consume the 'x' */
    n = read(pfd[0], b2, 1);
    CHECK("pipe read after close → 0 (EOF)", n == 0);
    close(pfd[0]);

    /* 4. write to pipe with closed reader → SIGPIPE / EPIPE */
    pipe(pfd);
    close(pfd[0]);
    signal(SIGPIPE, SIG_IGN);
    errno = 0;
    n = write(pfd[1], "y", 1);
    CHECK_ERRNO("write closed reader → EPIPE", n == -1, EPIPE);
    signal(SIGPIPE, SIG_DFL);
    close(pfd[1]);

    /* 5. pipe2 O_CLOEXEC */
    r = pipe2(pfd, O_CLOEXEC);
    CHECK("pipe2 O_CLOEXEC", r == 0);
    if (r == 0) {
        int f0 = fcntl(pfd[0], F_GETFD);
        int f1 = fcntl(pfd[1], F_GETFD);
        CHECK("pipe2 O_CLOEXEC flags", (f0 & FD_CLOEXEC) && (f1 & FD_CLOEXEC));
        close(pfd[0]); close(pfd[1]);
    }

    /* 6. pipe2 O_NONBLOCK */
    r = pipe2(pfd, O_NONBLOCK);
    CHECK("pipe2 O_NONBLOCK", r == 0);
    if (r == 0) {
        errno = 0;
        n = read(pfd[0], buf, 1);
        CHECK_ERRNO("nonblock read empty → EAGAIN", n == -1, EAGAIN);
        close(pfd[0]); close(pfd[1]);
    }

    /* 7. blocking read across fork */
    pipe(pfd);
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        usleep(10000); /* 10ms delay */
        write(pfd[1], "Z", 1);
        close(pfd[1]);
        _exit(0);
    }
    close(pfd[1]);
    char c = 0;
    n = read(pfd[0], &c, 1); /* should block until child writes */
    CHECK("blocking read across fork", n == 1 && c == 'Z');
    close(pfd[0]);
    waitpid(pid, NULL, 0);

    /* 8. pipe ordering — FIFO */
    pipe(pfd);
    write(pfd[1], "123", 3);
    write(pfd[1], "456", 3);
    char fifo[8] = {0};
    read(pfd[0], fifo, 6);
    CHECK("pipe FIFO order", memcmp(fifo, "123456", 6) == 0);
    close(pfd[0]); close(pfd[1]);

    /* 9. partial read */
    pipe(pfd);
    write(pfd[1], "ABCDE", 5);
    char p1[3] = {0}, p2[3] = {0};
    read(pfd[0], p1, 2);
    read(pfd[0], p2, 3);
    CHECK("pipe partial read", memcmp(p1, "AB", 2) == 0 && memcmp(p2, "CDE", 3) == 0);
    close(pfd[0]); close(pfd[1]);

    /* 10. large write + read through pipe */
    pipe(pfd);
    pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        char big[8192];
        memset(big, 'W', sizeof(big));
        ssize_t total = 0;
        while (total < (ssize_t)sizeof(big)) {
            ssize_t w = write(pfd[1], big + total, sizeof(big) - total);
            if (w <= 0) break;
            total += w;
        }
        close(pfd[1]);
        _exit(total == (ssize_t)sizeof(big) ? 0 : 1);
    }
    close(pfd[1]);
    char rbig[8192];
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(rbig)) {
        n = read(pfd[0], rbig + total, sizeof(rbig) - total);
        if (n <= 0) break;
        total += n;
    }
    CHECK("large pipe transfer 8K", total == 8192);
    close(pfd[0]);
    int st;
    waitpid(pid, &st, 0);

    /* 11. dup2 pipe fd */
    pipe(pfd);
    int dup_fd = dup(pfd[0]);
    CHECK("dup pipe read fd", dup_fd >= 0);
    write(pfd[1], "D", 1);
    char dc = 0;
    read(dup_fd, &dc, 1);
    CHECK("read from dup'd pipe fd", dc == 'D');
    close(dup_fd); close(pfd[0]); close(pfd[1]);

    LTP_SUMMARY("pipe");
}
