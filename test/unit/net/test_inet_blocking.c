/* TCP/UDP blocking-IO tests.
 *
 * net_tcp_t.wait_wq + udp_sock_t.recv_wq carry blocked recv/accept/connect
 * sleepers. Validates POSIX-correct multi-waiter wakeup + signal
 * interruption on AF_INET / AF_INET6 sockets:
 *   - blocking TCP recv wakes when peer sends
 *   - blocking TCP accept wakes when client connect()s
 *   - blocking TCP recv returns -EINTR on signal
 *   - blocking UDP recvfrom wakes when packet arrives
 */
#include "ktest.h"

#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define INADDR_LOOPBACK 0x0100007FU

#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

struct ksockaddr_in { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; };

struct ksigaction_inet {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

__attribute__((naked)) static void inet_sig_restorer(void) {
    __asm__ volatile("mov $15, %%rax\nsyscall\n" ::: "memory");
}

static void inet_sig_handler(int sig) { (void)sig; }

static void install_inet_handler(int signo, void (*fn)(int)) {
    struct ksigaction_inet sa = {
        .handler = (void *)fn,
        .flags = SA_RESTORER, /* no SA_RESTART → -ERESTARTSYS becomes -EINTR */
        .restorer = (void *)inet_sig_restorer,
        .mask = 0,
    };
    sc4(SYS_RT_SIGACTION, signo, (long)&sa, 0, 8);
}

static uint16_t port_be(uint16_t host) {
    return (uint16_t)((host >> 8) | (host << 8));
}

static void fill_loopback(struct ksockaddr_in *sa, uint16_t host_port) {
    for (int i = 0; i < (int)sizeof(*sa); i++) ((char *)sa)[i] = 0;
    sa->family = AF_INET;
    sa->port = port_be(host_port);
    sa->addr = INADDR_LOOPBACK;
}

/* ── 01: blocked TCP recv wakes on peer send ── */
static void test_tcp_block_recv_wakeup(void) {
    puts("\n[net/tcp blocking]\n");

    int lfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check_ge("recv_wakeup: listen socket", lfd, 0);
    if (lfd < 0) return;

    /* Bind ephemeral, then read out the assigned port via getsockname. */
    struct ksockaddr_in la;
    fill_loopback(&la, 0);
    long r = sc3(SYS_BIND, lfd, (long)&la, 16);
    check_val("recv_wakeup: bind", r, 0);

    struct ksockaddr_in ba;
    int balen = (int)sizeof(ba);
    r = sc3(SYS_GETSOCKNAME, lfd, (long)&ba, (long)&balen);
    check_val("recv_wakeup: getsockname", r, 0);

    r = sc2(SYS_LISTEN, lfd, 4);
    check_val("recv_wakeup: listen", r, 0);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        int cfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
        struct ksockaddr_in ca = ba; /* same port, loopback */
        ca.family = AF_INET; ca.addr = INADDR_LOOPBACK;
        long cr = sc3(SYS_CONNECT, cfd, (long)&ca, 16);
        if (cr != 0 && cr != -ERESTARTSYS) sc1(SYS_EXIT, 99);

        struct k_timespec t0, t1;
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t0);
        char buf[16] = {0};
        long rd = sc3(SYS_READ, cfd, (long)buf, 5);
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
        long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L +
                          (t1.tv_nsec - t0.tv_nsec) / 1000000L;
        int code = 0;
        if (rd == 5) code |= 1;
        if (buf[0] == 'P') code |= 2;
        if (buf[4] == 'g') code |= 4;
        long units = elapsed_ms / 10;
        if (units > 3) units = 3;
        code |= (int)(units << 3);
        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT, code);
        __builtin_unreachable();
    }

    int afd = (int)sc4(SYS_ACCEPT, lfd, 0, 0, 0);
    check_ge("recv_wakeup: accept", afd, 0);

    /* Sleep so child is parked in read() before we send. */
    struct k_timespec delay = { .tv_sec = 0, .tv_nsec = 30000000 };
    sc2(SYS_NANOSLEEP, (long)&delay, 0);

    long w = sc3(SYS_WRITE, afd, (long)"Pingg", 5);
    check_val("recv_wakeup: server write", w, 5);

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    int code = (ws >> 8) & 0xFF;
    check("recv_wakeup: child read 5 bytes", (code & 1) == 1);
    check("recv_wakeup: child saw 'P'",      (code & 2) == 2);
    check("recv_wakeup: child saw 'g'",      (code & 4) == 4);
    int units = (code >> 3) & 0x3;
    check("recv_wakeup: child blocked >= 20ms", units >= 2);

    sc1(SYS_CLOSE, afd);
    sc1(SYS_CLOSE, lfd);
}

/* ── 02: blocked TCP accept wakes on client connect ── */
static void test_tcp_block_accept_wakeup(void) {
    int lfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check_ge("accept_wakeup: listen socket", lfd, 0);
    if (lfd < 0) return;

    struct ksockaddr_in la;
    fill_loopback(&la, 0);
    long r = sc3(SYS_BIND, lfd, (long)&la, 16);
    check_val("accept_wakeup: bind", r, 0);

    struct ksockaddr_in ba;
    int balen = (int)sizeof(ba);
    r = sc3(SYS_GETSOCKNAME, lfd, (long)&ba, (long)&balen);
    check_val("accept_wakeup: getsockname", r, 0);

    r = sc2(SYS_LISTEN, lfd, 4);
    check_val("accept_wakeup: listen", r, 0);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct k_timespec t0, t1;
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t0);
        long afd = sc4(SYS_ACCEPT, lfd, 0, 0, 0);
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
        long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L +
                          (t1.tv_nsec - t0.tv_nsec) / 1000000L;
        int code = 0;
        if (afd >= 0) code |= 1;
        long units = elapsed_ms / 10;
        if (units > 3) units = 3;
        code |= (int)(units << 1);
        if (afd >= 0) sc1(SYS_CLOSE, afd);
        sc1(SYS_EXIT, code);
        __builtin_unreachable();
    }

    struct k_timespec delay = { .tv_sec = 0, .tv_nsec = 30000000 };
    sc2(SYS_NANOSLEEP, (long)&delay, 0);

    int cfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    struct ksockaddr_in ca = ba;
    ca.family = AF_INET; ca.addr = INADDR_LOOPBACK;
    long cr = sc3(SYS_CONNECT, cfd, (long)&ca, 16);
    check("accept_wakeup: connect", cr == 0 || cr == -ERESTARTSYS);

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    int code = (ws >> 8) & 0xFF;
    check("accept_wakeup: child accepted",      (code & 1) == 1);
    int units = (code >> 1) & 0x3;
    check("accept_wakeup: child blocked >= 20ms", units >= 2);

    sc1(SYS_CLOSE, cfd);
    sc1(SYS_CLOSE, lfd);
}

/* ── 03: signal interrupts blocking TCP recv ── */
static void test_tcp_block_recv_signal(void) {
    install_inet_handler(SIGUSR1, inet_sig_handler);

    int lfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check_ge("recv_signal: listen socket", lfd, 0);
    if (lfd < 0) return;

    struct ksockaddr_in la;
    fill_loopback(&la, 0);
    long r = sc3(SYS_BIND, lfd, (long)&la, 16);
    check_val("recv_signal: bind", r, 0);

    struct ksockaddr_in ba;
    int balen = (int)sizeof(ba);
    sc3(SYS_GETSOCKNAME, lfd, (long)&ba, (long)&balen);
    sc2(SYS_LISTEN, lfd, 4);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        install_inet_handler(SIGUSR1, inet_sig_handler);
        int cfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
        struct ksockaddr_in ca = ba;
        ca.family = AF_INET; ca.addr = INADDR_LOOPBACK;
        sc3(SYS_CONNECT, cfd, (long)&ca, 16);
        char buf[8] = {0};
        long rd = sc3(SYS_READ, cfd, (long)buf, 8);
        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT, (rd == -EINTR) ? 1 : 0);
        __builtin_unreachable();
    }

    int afd = (int)sc4(SYS_ACCEPT, lfd, 0, 0, 0);

    struct k_timespec delay = { .tv_sec = 0, .tv_nsec = 30000000 };
    sc2(SYS_NANOSLEEP, (long)&delay, 0);
    sc2(SYS_KILL, pid, SIGUSR1);

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("recv_signal: child got -EINTR", (ws >> 8) & 0xFF, 1);

    if (afd >= 0) sc1(SYS_CLOSE, afd);
    sc1(SYS_CLOSE, lfd);
}

/* ── 04: blocked UDP recvfrom wakes on packet ── */
static void test_udp_block_recvfrom_wakeup(void) {
    puts("\n[net/udp blocking]\n");

    int sfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_DGRAM, 0);
    check_ge("udp_recv_wakeup: server socket", sfd, 0);
    if (sfd < 0) return;

    struct ksockaddr_in la;
    fill_loopback(&la, 0);
    long r = sc3(SYS_BIND, sfd, (long)&la, 16);
    check_val("udp_recv_wakeup: bind", r, 0);

    struct ksockaddr_in ba;
    int balen = (int)sizeof(ba);
    r = sc3(SYS_GETSOCKNAME, sfd, (long)&ba, (long)&balen);
    check_val("udp_recv_wakeup: getsockname", r, 0);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct k_timespec t0, t1;
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t0);
        char buf[16] = {0};
        long rd = sc6(SYS_RECVFROM, sfd, (long)buf, 8, 0, 0, 0);
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
        long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L +
                          (t1.tv_nsec - t0.tv_nsec) / 1000000L;
        int code = 0;
        if (rd == 4) code |= 1;
        if (buf[0] == 'U') code |= 2;
        if (buf[3] == 'P') code |= 4;
        long units = elapsed_ms / 10;
        if (units > 3) units = 3;
        code |= (int)(units << 3);
        sc1(SYS_EXIT, code);
        __builtin_unreachable();
    }

    /* Parent: wait, then send to the bound port */
    struct k_timespec delay = { .tv_sec = 0, .tv_nsec = 30000000 };
    sc2(SYS_NANOSLEEP, (long)&delay, 0);

    int csock = (int)sc3(SYS_SOCKET, AF_INET, SOCK_DGRAM, 0);
    struct ksockaddr_in da = ba;
    da.family = AF_INET; da.addr = INADDR_LOOPBACK;
    long w = sc6(SYS_SENDTO, csock, (long)"UDXP", 4, 0, (long)&da, 16);
    check_val("udp_recv_wakeup: sendto", w, 4);

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    int code = (ws >> 8) & 0xFF;
    check("udp_recv_wakeup: child got 4 bytes", (code & 1) == 1);
    check("udp_recv_wakeup: child saw 'U'",     (code & 2) == 2);
    check("udp_recv_wakeup: child saw 'P'",     (code & 4) == 4);
    int units = (code >> 3) & 0x3;
    check("udp_recv_wakeup: child blocked >= 20ms", units >= 2);

    sc1(SYS_CLOSE, csock);
    sc1(SYS_CLOSE, sfd);
}

TEST("tcp/block_recv_wakeup",   test_tcp_block_recv_wakeup);
TEST("tcp/block_accept_wakeup", test_tcp_block_accept_wakeup);
TEST("tcp/block_recv_signal",   test_tcp_block_recv_signal);
TEST("udp/block_recvfrom_wakeup", test_udp_block_recvfrom_wakeup);
