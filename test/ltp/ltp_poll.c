/* LTP: poll/epoll_create/epoll_ctl/epoll_wait/select */
#define _GNU_SOURCE
#include "ltp.h"
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/select.h>

int main(void) {
    printf("=== LTP: poll ===\n");
    int pfd[2];

    /* 1. poll readable pipe */
    pipe(pfd);
    write(pfd[1], "X", 1);
    struct pollfd pf = { .fd = pfd[0], .events = POLLIN };
    int r = poll(&pf, 1, 100);
    CHECK("poll POLLIN ready", r == 1 && (pf.revents & POLLIN));
    close(pfd[0]); close(pfd[1]);

    /* 2. poll timeout (empty pipe) */
    pipe(pfd);
    pf = (struct pollfd){ .fd = pfd[0], .events = POLLIN };
    r = poll(&pf, 1, 0); /* immediate timeout */
    CHECK("poll timeout → 0", r == 0);
    close(pfd[0]); close(pfd[1]);

    /* 3. poll POLLHUP on closed writer */
    pipe(pfd);
    close(pfd[1]);
    pf = (struct pollfd){ .fd = pfd[0], .events = POLLIN };
    r = poll(&pf, 1, 100);
    CHECK("poll POLLHUP closed writer", r >= 1 && (pf.revents & (POLLHUP | POLLIN)));
    close(pfd[0]);

    /* 4. poll writable pipe */
    pipe(pfd);
    pf = (struct pollfd){ .fd = pfd[1], .events = POLLOUT };
    r = poll(&pf, 1, 100);
    CHECK("poll POLLOUT writable", r == 1 && (pf.revents & POLLOUT));
    close(pfd[0]); close(pfd[1]);

    /* 5. poll bad fd → POLLNVAL */
    pf = (struct pollfd){ .fd = 9999, .events = POLLIN };
    r = poll(&pf, 1, 0);
    CHECK("poll bad fd → POLLNVAL", r == 1 && (pf.revents & POLLNVAL));

    /* 6. epoll_create */
    int epfd = epoll_create1(0);
    CHECK("epoll_create1", epfd >= 0);

    /* 7. epoll_ctl ADD + epoll_wait */
    pipe(pfd);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = pfd[0] };
    r = epoll_ctl(epfd, EPOLL_CTL_ADD, pfd[0], &ev);
    CHECK("epoll_ctl ADD", r == 0);

    write(pfd[1], "E", 1);
    struct epoll_event out;
    r = epoll_wait(epfd, &out, 1, 100);
    CHECK("epoll_wait ready", r == 1 && (out.events & EPOLLIN) && out.data.fd == pfd[0]);

    /* 8. epoll_ctl MOD */
    ev.events = EPOLLOUT;
    r = epoll_ctl(epfd, EPOLL_CTL_MOD, pfd[0], &ev);
    CHECK("epoll_ctl MOD", r == 0);

    /* 9. epoll_ctl DEL */
    r = epoll_ctl(epfd, EPOLL_CTL_DEL, pfd[0], NULL);
    CHECK("epoll_ctl DEL", r == 0);
    close(pfd[0]); close(pfd[1]); close(epfd);

    /* 10. epoll_create1 EPOLL_CLOEXEC */
    epfd = epoll_create1(EPOLL_CLOEXEC);
    CHECK("epoll_create1 CLOEXEC", epfd >= 0);
    if (epfd >= 0) {
        int flags = fcntl(epfd, F_GETFD);
        CHECK("epoll CLOEXEC flag", (flags & FD_CLOEXEC) != 0);
        close(epfd);
    }

    /* 11. epoll_wait timeout */
    epfd = epoll_create1(0);
    pipe(pfd);
    ev = (struct epoll_event){ .events = EPOLLIN, .data.fd = pfd[0] };
    epoll_ctl(epfd, EPOLL_CTL_ADD, pfd[0], &ev);
    r = epoll_wait(epfd, &out, 1, 0); /* immediate */
    CHECK("epoll_wait timeout → 0", r == 0);
    close(pfd[0]); close(pfd[1]); close(epfd);

    /* 12. select readable */
    pipe(pfd);
    write(pfd[1], "S", 1);
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(pfd[0], &rfds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    r = select(pfd[0] + 1, &rfds, NULL, NULL, &tv);
    CHECK("select readable", r == 1 && FD_ISSET(pfd[0], &rfds));
    close(pfd[0]); close(pfd[1]);

    LTP_SUMMARY("poll");
}
