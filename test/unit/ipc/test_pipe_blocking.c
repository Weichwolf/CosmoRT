/* Phase 10.2b-2 — pipe waitqueue migration tests.
 *
 * pipe.read_wq / pipe.write_wq replaced single-slot blocked_reader/writer.
 * Validates POSIX-correct multi-waiter wakeup + signal interruption:
 *   - blocking read wakes when writer pushes data
 *   - blocking read returns -EINTR on signal (no SA_RESTART)
 *   - writer blocks when ring is full, drains on reader read
 *   - two concurrent readers each receive one message (multi-reader fairness)
 */
#include "ktest.h"

struct ksigaction_pipe {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

__attribute__((naked)) static void pipe_sig_restorer(void) {
    __asm__ volatile("mov $15, %%rax\nsyscall\n" ::: "memory");
}

static void pipe_sig_handler(int sig) { (void)sig; }

static void install_pipe_handler(int signo, void (*fn)(int)) {
    struct ksigaction_pipe sa = {
        .handler = (void *)fn,
        .flags = SA_RESTORER,   /* no SA_RESTART → -ERESTARTSYS becomes -EINTR */
        .restorer = (void *)pipe_sig_restorer,
        .mask = 0,
    };
    sc4(SYS_RT_SIGACTION, signo, (long)&sa, 0, 8);
}

/* ── 01: blocked reader wakes on write ─────────── */
static void test_pipe_block_read_wakeup(void) {
    puts("\n[pipe blocking]\n");
    int fds[2] = {-1, -1};
    long r = sc2(SYS_PIPE2, (long)fds, 0);
    check_val("block_read: pipe2 returns 0", r, 0);
    if (r < 0) return;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct k_timespec t0, t1;
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t0);
        char buf[8] = {0};
        long rd = sc3(SYS_READ, fds[0], (long)buf, 8);
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
        long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L +
                          (t1.tv_nsec - t0.tv_nsec) / 1000000L;
        /* bit0 = read 5, bit1 = buf[0]=='H', bit2 = buf[4]=='o', bits3-4 = 10ms units cap 3 */
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
    long w = sc3(SYS_WRITE, fds[1], (long)"Hello", 5);
    check_val("block_read: parent write returned 5", w, 5);

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    int code = (ws >> 8) & 0xFF;
    check("block_read: child read 5 bytes", (code & 1) == 1);
    check("block_read: child saw 'H'",     (code & 2) == 2);
    check("block_read: child saw 'o'",     (code & 4) == 4);
    int units = (code >> 3) & 0x3;
    check("block_read: child blocked >= 20ms", units >= 2);

    sc1(SYS_CLOSE, fds[0]);
    sc1(SYS_CLOSE, fds[1]);
}

/* ── 02: signal interrupts blocking read ─────── */
static void test_pipe_block_read_signal(void) {
    install_pipe_handler(SIGUSR1, pipe_sig_handler);

    int fds[2] = {-1, -1};
    long r = sc2(SYS_PIPE2, (long)fds, 0);
    check_val("block_signal: pipe2 returns 0", r, 0);
    if (r < 0) return;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        install_pipe_handler(SIGUSR1, pipe_sig_handler);
        char buf[8] = {0};
        long rd = sc3(SYS_READ, fds[0], (long)buf, 8);
        sc1(SYS_EXIT, (rd == -EINTR) ? 1 : 0);
        __builtin_unreachable();
    }

    struct k_timespec delay = { .tv_sec = 0, .tv_nsec = 30000000 };
    sc2(SYS_NANOSLEEP, (long)&delay, 0);
    sc2(SYS_KILL, pid, SIGUSR1);

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("block_signal: child got -EINTR", (ws >> 8) & 0xFF, 1);

    sc1(SYS_CLOSE, fds[0]);
    sc1(SYS_CLOSE, fds[1]);
}

/* ── 03: blocked writer wakes when reader drains ──
 * Pipe default = 64 KiB. Fill with one big write → next write blocks. */
static void test_pipe_block_write_wakeup(void) {
    int fds[2] = {-1, -1};
    long r = sc2(SYS_PIPE2, (long)fds, 0);
    check_val("block_write: pipe2 returns 0", r, 0);
    if (r < 0) return;

    /* Saturate ring with 64 KiB of zero bytes from a static buffer. */
    static char zeros[65536];
    long w = sc3(SYS_WRITE, fds[1], (long)zeros, 65536);
    check_val("block_write: pre-fill returned 65536", w, 65536);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct k_timespec t0, t1;
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t0);
        long w2 = sc3(SYS_WRITE, fds[1], (long)"X", 1);
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
        long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L +
                          (t1.tv_nsec - t0.tv_nsec) / 1000000L;
        int code = 0;
        if (w2 == 1) code |= 1;
        long units = elapsed_ms / 10;
        if (units > 3) units = 3;
        code |= (int)(units << 1);
        sc1(SYS_EXIT, code);
        __builtin_unreachable();
    }

    struct k_timespec delay = { .tv_sec = 0, .tv_nsec = 30000000 };
    sc2(SYS_NANOSLEEP, (long)&delay, 0);
    /* Drain the entire 64 KiB so the blocked writer can progress. */
    static char drain[65536];
    long rd = sc3(SYS_READ, fds[0], (long)drain, 65536);
    check_val("block_write: drain read 65536", rd, 65536);

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    int code = (ws >> 8) & 0xFF;
    check("block_write: child wrote 1 byte", (code & 1) == 1);
    int units = (code >> 1) & 0x3;
    check("block_write: child blocked >= 20ms", units >= 2);

    sc1(SYS_CLOSE, fds[0]);
    sc1(SYS_CLOSE, fds[1]);
}

/* ── 04: two readers, each gets exactly one message ──
 * POSIX correctness: single-slot blocked_reader would have lost one wakeup. */
static void test_pipe_multiple_readers(void) {
    int fds[2] = {-1, -1};
    long r = sc2(SYS_PIPE2, (long)fds, 0);
    check_val("multi_read: pipe2 returns 0", r, 0);
    if (r < 0) return;

    long c1 = sc0(SYS_FORK);
    if (c1 == 0) {
        char b[4] = {0};
        long rd = sc3(SYS_READ, fds[0], (long)b, 1);
        sc1(SYS_EXIT, (rd == 1) ? (int)b[0] : 0);
        __builtin_unreachable();
    }
    long c2 = sc0(SYS_FORK);
    if (c2 == 0) {
        char b[4] = {0};
        long rd = sc3(SYS_READ, fds[0], (long)b, 1);
        sc1(SYS_EXIT, (rd == 1) ? (int)b[0] : 0);
        __builtin_unreachable();
    }

    /* Both children block in read; stagger writes 30ms apart to make
     * ordering deterministic — first wake_up routes to head of read_wq,
     * second write_wakes the remaining waiter. */
    struct k_timespec delay = { .tv_sec = 0, .tv_nsec = 30000000 };
    sc2(SYS_NANOSLEEP, (long)&delay, 0);
    long w1 = sc3(SYS_WRITE, fds[1], (long)"A", 1);
    check_val("multi_read: first write returned 1", w1, 1);
    sc2(SYS_NANOSLEEP, (long)&delay, 0);
    long w2 = sc3(SYS_WRITE, fds[1], (long)"B", 1);
    check_val("multi_read: second write returned 1", w2, 1);

    int ws1 = 0, ws2 = 0;
    sc4(SYS_WAIT4, c1, (long)&ws1, 0, 0);
    sc4(SYS_WAIT4, c2, (long)&ws2, 0, 0);
    int got1 = (ws1 >> 8) & 0xFF;
    int got2 = (ws2 >> 8) & 0xFF;
    /* Each child must report a non-zero byte; sum must be 'A'+'B'. */
    check("multi_read: child1 got a byte", got1 == 'A' || got1 == 'B');
    check("multi_read: child2 got a byte", got2 == 'A' || got2 == 'B');
    check("multi_read: distinct bytes",    got1 != got2);

    sc1(SYS_CLOSE, fds[0]);
    sc1(SYS_CLOSE, fds[1]);
}

TEST("pipe/block_read_wakeup",  test_pipe_block_read_wakeup);
TEST("pipe/block_read_signal",  test_pipe_block_read_signal);
TEST("pipe/block_write_wakeup", test_pipe_block_write_wakeup);
TEST("pipe/multiple_readers",   test_pipe_multiple_readers);
