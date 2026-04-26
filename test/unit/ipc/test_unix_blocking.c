/* Phase 10.2b-3 — AF_UNIX waitqueue migration tests.
 *
 * usock->{read,write,accept,connect}_wq replaced single-slot blocked_reader/
 * _acceptor + per-thread event_queue. Validates POSIX-correct multi-waiter
 * wakeup + signal interruption:
 *   - blocking read on socketpair wakes when peer writes
 *   - blocking read returns -EINTR on signal (no SA_RESTART)
 *   - blocking accept wakes when client connect()s
 */
#include "ktest.h"

#define AF_UNIX     1
#define SOCK_STREAM 1

struct ksigaction_usock {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

__attribute__((naked)) static void usock_sig_restorer(void) {
    __asm__ volatile("mov $15, %%rax\nsyscall\n" ::: "memory");
}

static void usock_sig_handler(int sig) { (void)sig; }

static void install_usock_handler(int signo, void (*fn)(int)) {
    struct ksigaction_usock sa = {
        .handler = (void *)fn,
        .flags = SA_RESTORER, /* no SA_RESTART → -ERESTARTSYS becomes -EINTR */
        .restorer = (void *)usock_sig_restorer,
        .mask = 0,
    };
    sc4(SYS_RT_SIGACTION, signo, (long)&sa, 0, 8);
}

/* Abstract-namespace AF_UNIX address (path[0]==0) with unique tag. */
struct ksun { uint16_t family; char path[108]; };

static int make_abstract(struct ksun *out, const char *tag) {
    out->family = AF_UNIX;
    out->path[0] = 0;
    int i = 1;
    while (tag[i - 1] && i < 14) { out->path[i] = tag[i - 1]; i++; }
    return 2 + i;
}

/* ── 01: blocked reader on socketpair wakes on peer write ── */
static void test_unix_block_read_wakeup(void) {
    puts("\n[unix blocking]\n");
    int sv[2] = { -1, -1 };
    long r = sc4(SYS_SOCKETPAIR, AF_UNIX, SOCK_STREAM, 0, (long)sv);
    check_val("block_read: socketpair", r, 0);
    if (r != 0) return;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct k_timespec t0, t1;
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t0);
        char buf[8] = {0};
        long rd = sc3(SYS_READ, sv[1], (long)buf, 8);
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
        long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L +
                          (t1.tv_nsec - t0.tv_nsec) / 1000000L;
        int code = 0;
        if (rd == 5) code |= 1;
        if (buf[0] == 'H') code |= 2;
        if (buf[4] == 'o') code |= 4;
        long units = elapsed_ms / 10;
        if (units > 3) units = 3;
        code |= (int)(units << 3);
        sc1(SYS_EXIT, code);
        __builtin_unreachable();
    }

    struct k_timespec delay = { .tv_sec = 0, .tv_nsec = 30000000 };
    sc2(SYS_NANOSLEEP, (long)&delay, 0);
    long w = sc3(SYS_WRITE, sv[0], (long)"Hello", 5);
    check_val("block_read: parent write returned 5", w, 5);

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    int code = (ws >> 8) & 0xFF;
    check("block_read: child read 5 bytes", (code & 1) == 1);
    check("block_read: child saw 'H'",     (code & 2) == 2);
    check("block_read: child saw 'o'",     (code & 4) == 4);
    int units = (code >> 3) & 0x3;
    check("block_read: child blocked >= 20ms", units >= 2);

    sc1(SYS_CLOSE, sv[0]);
    sc1(SYS_CLOSE, sv[1]);
}

/* ── 02: signal interrupts blocking read on socketpair ── */
static void test_unix_block_read_signal(void) {
    install_usock_handler(SIGUSR1, usock_sig_handler);

    int sv[2] = { -1, -1 };
    long r = sc4(SYS_SOCKETPAIR, AF_UNIX, SOCK_STREAM, 0, (long)sv);
    check_val("block_signal: socketpair", r, 0);
    if (r != 0) return;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        install_usock_handler(SIGUSR1, usock_sig_handler);
        char buf[8] = {0};
        long rd = sc3(SYS_READ, sv[1], (long)buf, 8);
        sc1(SYS_EXIT, (rd == -EINTR) ? 1 : 0);
        __builtin_unreachable();
    }

    struct k_timespec delay = { .tv_sec = 0, .tv_nsec = 30000000 };
    sc2(SYS_NANOSLEEP, (long)&delay, 0);
    sc2(SYS_KILL, pid, SIGUSR1);

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("block_signal: child got -EINTR", (ws >> 8) & 0xFF, 1);

    sc1(SYS_CLOSE, sv[0]);
    sc1(SYS_CLOSE, sv[1]);
}

/* ── 03: blocked accept wakes on client connect ── */
static void test_unix_block_accept_wakeup(void) {
    int lfd = (int)sc3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
    check_ge("block_accept: listen socket", lfd, 0);
    if (lfd < 0) return;

    struct ksun la;
    int alen = make_abstract(&la, "blk_acc_001");
    long r = sc3(SYS_BIND, lfd, (long)&la, alen);
    check_val("block_accept: bind", r, 0);
    r = sc2(SYS_LISTEN, lfd, 4);
    check_val("block_accept: listen", r, 0);

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
    int cfd = (int)sc3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
    long cr = sc3(SYS_CONNECT, cfd, (long)&la, alen);
    check("block_accept: connect ok", cr == 0 || cr == -ERESTARTSYS);

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    int code = (ws >> 8) & 0xFF;
    check("block_accept: child accepted", (code & 1) == 1);
    int units = (code >> 1) & 0x3;
    check("block_accept: child blocked >= 20ms", units >= 2);

    sc1(SYS_CLOSE, cfd);
    sc1(SYS_CLOSE, lfd);
}

TEST("unix/block_read_wakeup",   test_unix_block_read_wakeup);
TEST("unix/block_read_signal",   test_unix_block_read_signal);
TEST("unix/block_accept_wakeup", test_unix_block_accept_wakeup);
