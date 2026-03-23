/* LTP: socket/bind/listen/accept/connect (AF_UNIX + AF_INET loopback) */
#define _GNU_SOURCE
#include "ltp.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>

int main(void) {
    printf("=== LTP: socket ===\n");

    /* 1. socket AF_UNIX SOCK_STREAM */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK("socket AF_UNIX STREAM", fd >= 0);
    if (fd >= 0) close(fd);

    /* 2. socket AF_INET SOCK_STREAM */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK("socket AF_INET STREAM", fd >= 0);
    if (fd >= 0) close(fd);

    /* 3. socket AF_INET SOCK_DGRAM */
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK("socket AF_INET DGRAM", fd >= 0);
    if (fd >= 0) close(fd);

    /* 4. socket bad domain → EAFNOSUPPORT */
    errno = 0;
    fd = socket(9999, SOCK_STREAM, 0);
    CHECK_ERRNO("socket bad domain", fd == -1, EAFNOSUPPORT);

    /* 5. AF_UNIX connect/accept pair */
    {
        const char *path = "/tmp/ltp_sock_test";
        unlink(path);
        int srv = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
        int r = bind(srv, (struct sockaddr *)&addr, sizeof(addr));
        CHECK("AF_UNIX bind", r == 0);
        r = listen(srv, 5);
        CHECK("AF_UNIX listen", r == 0);

        pid_t pid = fork();
        if (pid == 0) {
            close(srv);
            int cli = socket(AF_UNIX, SOCK_STREAM, 0);
            connect(cli, (struct sockaddr *)&addr, sizeof(addr));
            write(cli, "UNIX", 4);
            close(cli);
            _exit(0);
        }
        int cli = accept(srv, NULL, NULL);
        CHECK("AF_UNIX accept", cli >= 0);
        char buf[8] = {0};
        if (cli >= 0) {
            read(cli, buf, 4);
            CHECK("AF_UNIX recv data", memcmp(buf, "UNIX", 4) == 0);
            close(cli);
        }
        close(srv);
        unlink(path);
        waitpid(pid, NULL, 0);
    }

    /* 6. AF_INET loopback TCP connect/accept */
    {
        int srv = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(0), /* kernel picks port */
            .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        };
        bind(srv, (struct sockaddr *)&addr, sizeof(addr));
        socklen_t alen = sizeof(addr);
        getsockname(srv, (struct sockaddr *)&addr, &alen);
        int port = ntohs(addr.sin_port);
        CHECK("AF_INET bind ephemeral", port > 0);
        listen(srv, 5);

        pid_t pid = fork();
        if (pid == 0) {
            close(srv);
            int cli = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in ca = {
                .sin_family = AF_INET,
                .sin_port = htons(port),
                .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
            };
            int cr = connect(cli, (struct sockaddr *)&ca, sizeof(ca));
            if (cr == 0) write(cli, "TCP!", 4);
            close(cli);
            _exit(cr == 0 ? 0 : 1);
        }
        int cli = accept(srv, NULL, NULL);
        CHECK("AF_INET accept", cli >= 0);
        char buf[8] = {0};
        if (cli >= 0) {
            read(cli, buf, 4);
            CHECK("AF_INET recv data", memcmp(buf, "TCP!", 4) == 0);
            close(cli);
        }
        close(srv);
        int st;
        waitpid(pid, &st, 0);
    }

    /* 7. sendto/recvfrom UDP */
    {
        int s1 = socket(AF_INET, SOCK_DGRAM, 0);
        int s2 = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a1 = {
            .sin_family = AF_INET,
            .sin_port = htons(0),
            .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        };
        bind(s1, (struct sockaddr *)&a1, sizeof(a1));
        socklen_t alen = sizeof(a1);
        getsockname(s1, (struct sockaddr *)&a1, &alen);

        sendto(s2, "UDP", 3, 0, (struct sockaddr *)&a1, sizeof(a1));
        char buf[8] = {0};
        ssize_t n = recvfrom(s1, buf, sizeof(buf), 0, NULL, NULL);
        CHECK("UDP sendto/recvfrom", n == 3 && memcmp(buf, "UDP", 3) == 0);
        close(s1); close(s2);
    }

    /* 8. socketpair */
    {
        int sv[2];
        int r = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        CHECK("socketpair AF_UNIX", r == 0);
        if (r == 0) {
            write(sv[0], "P", 1);
            char c = 0;
            read(sv[1], &c, 1);
            CHECK("socketpair bidir", c == 'P');
            close(sv[0]); close(sv[1]);
        }
    }

    /* 9. shutdown SHUT_WR → peer reads EOF */
    {
        int sv[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        write(sv[0], "Q", 1);
        shutdown(sv[0], SHUT_WR);
        char buf[4] = {0};
        read(sv[1], buf, 1);
        ssize_t n = read(sv[1], buf + 1, 1);
        CHECK("shutdown SHUT_WR → EOF", buf[0] == 'Q' && n == 0);
        close(sv[0]); close(sv[1]);
    }

    /* 10. SOCK_CLOEXEC flag */
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK("SOCK_CLOEXEC", fd >= 0);
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFD);
        CHECK("SOCK_CLOEXEC → FD_CLOEXEC", (flags & FD_CLOEXEC) != 0);
        close(fd);
    }

    /* 11. connect to refused port → ECONNREFUSED */
    {
        int cli = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(1), /* port 1 — almost certainly not listening */
            .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        };
        errno = 0;
        int r = connect(cli, (struct sockaddr *)&addr, sizeof(addr));
        CHECK_ERRNO("connect refused → ECONNREFUSED", r == -1, ECONNREFUSED);
        close(cli);
    }

    LTP_SUMMARY("socket");
}
