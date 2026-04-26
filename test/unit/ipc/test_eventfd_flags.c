/* Phase 10.2b-1 — eventfd waitqueue migration tests.
 *
 * Validates flags + blocking semantics on the new read_wq/write_wq:
 *   - reader blocks until writer increments counter
 *   - blocked reader returns -EINTR on signal
 *   - writer blocks at MAX, reader drains, writer completes
 */
#include "ktest.h"

struct ksigaction_efd {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

__attribute__((naked)) static void efd_sig_restorer(void) {
    __asm__ volatile("mov $15, %%rax\nsyscall\n" ::: "memory");
}

static void efd_sig_handler(int sig) { (void)sig; }

static void install_efd_handler(int signo, void (*fn)(int)) {
    struct ksigaction_efd sa = {
        .handler = (void *)fn,
        .flags = SA_RESTORER,
        .restorer = (void *)efd_sig_restorer,
        .mask = 0,
    };
    sc4(SYS_RT_SIGACTION, signo, (long)&sa, 0, 8);
}

static void test_eventfd_flags(void) {
    puts("\n[eventfd flags]\n");

    /* eventfd2 with EFD_CLOEXEC */
    long efd = sc2(SYS_EVENTFD2, 0, (long)EFD_CLOEXEC);
    check("eventfd2(EFD_CLOEXEC) >= 0", efd >= 0);
    if (efd >= 0) {
        long fd_flags = sc3(SYS_FCNTL, efd, F_GETFD, 0);
        check_val("eventfd CLOEXEC: F_GETFD = 1", fd_flags, FD_CLOEXEC);
        sc1(SYS_CLOSE, efd);
    }

    /* eventfd2 with EFD_NONBLOCK */
    efd = sc2(SYS_EVENTFD2, 0, (long)EFD_NONBLOCK);
    check("eventfd2(EFD_NONBLOCK) >= 0", efd >= 0);
    if (efd >= 0) {
        long fl = sc3(SYS_FCNTL, efd, F_GETFL, 0);
        check("eventfd NONBLOCK: O_NONBLOCK set", (fl & O_NONBLOCK) != 0);
        sc1(SYS_CLOSE, efd);
    }

    /* eventfd2 with both flags */
    efd = sc2(SYS_EVENTFD2, 0, (long)(EFD_CLOEXEC | EFD_NONBLOCK));
    check("eventfd2(CLOEXEC|NONBLOCK) >= 0", efd >= 0);
    if (efd >= 0) {
        long fd_flags = sc3(SYS_FCNTL, efd, F_GETFD, 0);
        check_val("eventfd both: F_GETFD = 1", fd_flags, FD_CLOEXEC);
        long fl = sc3(SYS_FCNTL, efd, F_GETFL, 0);
        check("eventfd both: O_NONBLOCK set", (fl & O_NONBLOCK) != 0);
        check("eventfd: F_GETFL excludes CLOEXEC", (fl & O_CLOEXEC) == 0);
        sc1(SYS_CLOSE, efd);
    }

    /* eventfd2 with no flags */
    efd = sc2(SYS_EVENTFD2, 0, 0);
    check("eventfd2(0) >= 0", efd >= 0);
    if (efd >= 0) {
        long fd_flags = sc3(SYS_FCNTL, efd, F_GETFD, 0);
        check_val("eventfd(0): F_GETFD = 0", fd_flags, 0);
        sc1(SYS_CLOSE, efd);
    }
}

/* ── 02: reader blocks until writer wakes ──
 * Child blocks in read; parent waits ~30ms, writes 7, wait4-collects child. */
static void test_eventfd_block_read_wakeup(void) {
    long efd = sc2(SYS_EVENTFD2, 0, 0);
    check("block_read: eventfd2 >= 0", efd >= 0);
    if (efd < 0) return;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct k_timespec t0, t1;
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t0);
        uint64_t val = 0;
        long r = sc3(SYS_READ, efd, (long)&val, 8);
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
        long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L +
                          (t1.tv_nsec - t0.tv_nsec) / 1000000L;
        /* bit0 = read 8, bit1 = val == 7, bits2-3 = 10ms units cap 3 */
        int code = 0;
        if (r == 8)         code |= 1;
        if (val == 7)       code |= 2;
        long units = elapsed_ms / 10;
        if (units > 3) units = 3;
        code |= (int)(units << 2);
        sc1(SYS_EXIT, code);
        __builtin_unreachable();
    }

    /* Wait long enough for the child to enter the blocking read. */
    struct k_timespec delay = { .tv_sec = 0, .tv_nsec = 30000000 };
    sc2(SYS_NANOSLEEP, (long)&delay, 0);
    uint64_t v = 7;
    long w = sc3(SYS_WRITE, efd, (long)&v, 8);
    check_val("block_read: write returned 8", w, 8);

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    int code = (ws >> 8) & 0xFF;
    check("block_read: child read 8", (code & 1) == 1);
    check("block_read: child got value 7", (code & 2) == 2);
    int units = (code >> 2) & 0x3;
    check("block_read: child blocked >= 20ms", units >= 2);

    sc1(SYS_CLOSE, efd);
}

/* ── 03: signal interrupts blocked read with -EINTR ── */
static void test_eventfd_block_read_signal(void) {
    install_efd_handler(SIGUSR1, efd_sig_handler);

    long efd = sc2(SYS_EVENTFD2, 0, 0);
    check("block_signal: eventfd2 >= 0", efd >= 0);
    if (efd < 0) return;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        install_efd_handler(SIGUSR1, efd_sig_handler);
        uint64_t val = 0;
        long r = sc3(SYS_READ, efd, (long)&val, 8);
        sc1(SYS_EXIT, (r == -EINTR) ? 1 : 0);
        __builtin_unreachable();
    }

    struct k_timespec delay = { .tv_sec = 0, .tv_nsec = 30000000 };
    sc2(SYS_NANOSLEEP, (long)&delay, 0);
    sc2(SYS_KILL, pid, SIGUSR1);

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("block_signal: child returned -EINTR", (ws >> 8) & 0xFF, 1);

    sc1(SYS_CLOSE, efd);
}

/* ── 04: writer blocks at MAX, reader drains, writer completes ──
 * Init counter to (uint64_t)-2 == EVENTFD_MAX_VAL. Any non-zero write blocks.
 * Child writes 1 (must block); parent reads (drains to 0), child wakes. */
static void test_eventfd_block_write_wakeup(void) {
    /* initval is unsigned int (32-bit) per syscall ABI; saturate counter via
     * read-then-write trick. Actually do_eventfd2 takes initval as u32 — so
     * we cannot fill to MAX directly. Instead: use EFD_SEMAPHORE + initval
     * MAX_INT and rely on the write itself adding val to counter. We can't
     * preload MAX, so test with direct overflow: write huge val, then write
     * another to force overflow path. */
    long efd = sc2(SYS_EVENTFD2, (long)0xFFFFFFFFu, 0);
    check("block_write: eventfd2 >= 0", efd >= 0);
    if (efd < 0) return;

    /* Push to within range of MAX: counter = 2^32 - 1 < MAX = 2^64 - 2.
     * One more write of (MAX - counter) would saturate. We need to write a
     * value that overflows. Easiest: write 0xFFFFFFFFFFFFFFFE - val to get
     * exactly to MAX, then write 1 to trigger -EAGAIN/block.
     * Two sequential writes accumulate. */
    uint64_t fill = 0xFFFFFFFFFFFFFFFEULL - 0xFFFFFFFFu;
    long w = sc3(SYS_WRITE, efd, (long)&fill, 8);
    check_val("block_write: fill to MAX returned 8", w, 8);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct k_timespec t0, t1;
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t0);
        uint64_t v = 1;
        long r = sc3(SYS_WRITE, efd, (long)&v, 8);
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
        long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L +
                          (t1.tv_nsec - t0.tv_nsec) / 1000000L;
        int code = 0;
        if (r == 8) code |= 1;
        long units = elapsed_ms / 10;
        if (units > 3) units = 3;
        code |= (int)(units << 1);
        sc1(SYS_EXIT, code);
        __builtin_unreachable();
    }

    struct k_timespec delay = { .tv_sec = 0, .tv_nsec = 30000000 };
    sc2(SYS_NANOSLEEP, (long)&delay, 0);
    /* Drain counter to 0 — wakes writer. */
    uint64_t got = 0;
    long r = sc3(SYS_READ, efd, (long)&got, 8);
    check_val("block_write: drain read returned 8", r, 8);

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    int code = (ws >> 8) & 0xFF;
    check("block_write: child write returned 8", (code & 1) == 1);
    int units = (code >> 1) & 0x3;
    check("block_write: child blocked >= 20ms", units >= 2);

    sc1(SYS_CLOSE, efd);
}

TEST("eventfd_flags",                test_eventfd_flags);
TEST("eventfd/block_read_wakeup",    test_eventfd_block_read_wakeup);
TEST("eventfd/block_read_signal",    test_eventfd_block_read_signal);
TEST("eventfd/block_write_wakeup",   test_eventfd_block_write_wakeup);
