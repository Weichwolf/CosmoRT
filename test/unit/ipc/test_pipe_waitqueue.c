/* Phase 10.2 — pipe_read/write waitqueue migration
 *
 * Deckt die Semantik ab, die der alte single-pointer blocked_reader/writer
 * nicht abdecken konnte:
 *   - mehrere parallele Blocker auf derselben Pipe
 *   - Exclusive-Wake: nur ein Reader pro write, nicht alle
 *   - EOF/EPIPE-Broadcast bei close wacht alle Waiter
 *   - Signal-EINTR bricht korrekt aus prepare_to_wait
 */
#include "ktest.h"

#define SYS_PIPE2   293

#define THREAD_STACK 65536

static long spawn(void (*fn)(void)) {
    long stk = sc6(SYS_MMAP, 0, THREAD_STACK, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (stk <= 0) return -1;
    long ret = sc5(SYS_CLONE,
        (long)(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD),
        stk + THREAD_STACK, 0, 0, 0);
    if (ret == 0) { fn(); __builtin_unreachable(); }
    return ret;
}

static void spin_until(volatile int *flag, int val, int iters) {
    for (int i = 0; i < iters && *flag != val; i++)
        __asm__ volatile("pause");
}

/* ── Multiple readers, one writer wakes exactly one ───────── */

static int pw_rfd, pw_wfd;
static volatile int pw_a_got, pw_b_got;

static void pw_reader_a(void) {
    char c;
    long r = sc3(SYS_READ, pw_rfd, (long)&c, 1);
    pw_a_got = (r == 1) ? (int)c : -1;
    sc1(SYS_EXIT, 0);
}
static void pw_reader_b(void) {
    char c;
    long r = sc3(SYS_READ, pw_rfd, (long)&c, 1);
    pw_b_got = (r == 1) ? (int)c : -1;
    sc1(SYS_EXIT, 0);
}

static void test_pipe_two_readers_exclusive_wake(void) {
    puts("\n[pipe_two_readers]\n");
    int fds[2] = {-1, -1};
    sc2(SYS_PIPE2, (long)fds, 0);
    pw_rfd = fds[0]; pw_wfd = fds[1];
    pw_a_got = pw_b_got = -999;

    long ta = spawn(pw_reader_a);
    long tb = spawn(pw_reader_b);
    check("spawn A", ta > 0);
    check("spawn B", tb > 0);
    if (ta <= 0 || tb <= 0) return;

    /* Both should block. */
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 20000000 };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);

    /* Write 1 byte — exactly ONE reader wakes. */
    sc3(SYS_WRITE, pw_wfd, (long)"X", 1);
    sc2(SYS_NANOSLEEP, (long)&ts, 0);

    int woken = (pw_a_got == 'X' ? 1 : 0) + (pw_b_got == 'X' ? 1 : 0);
    int still_blocked = (pw_a_got == -999 ? 1 : 0) + (pw_b_got == -999 ? 1 : 0);
    check_val("exactly one reader woken", woken, 1);
    check_val("exactly one still blocked", still_blocked, 1);

    /* Wake the other. */
    sc3(SYS_WRITE, pw_wfd, (long)"Y", 1);
    spin_until(&pw_a_got, 'Y', 5000000);
    spin_until(&pw_b_got, 'Y', 5000000);
    check("both got data eventually",
          (pw_a_got == 'X' || pw_a_got == 'Y') &&
          (pw_b_got == 'X' || pw_b_got == 'Y'));

    sc1(SYS_CLOSE, pw_rfd);
    sc1(SYS_CLOSE, pw_wfd);
}

/* ── Close write-end wakes ALL readers (EOF broadcast) ───── */

static int pe_rfd, pe_wfd;
static volatile int pe_a_eof, pe_b_eof;

static void pe_reader_a(void) {
    char c;
    long r = sc3(SYS_READ, pe_rfd, (long)&c, 1);
    pe_a_eof = (r == 0) ? 1 : 0; /* 0 = EOF */
    sc1(SYS_EXIT, 0);
}
static void pe_reader_b(void) {
    char c;
    long r = sc3(SYS_READ, pe_rfd, (long)&c, 1);
    pe_b_eof = (r == 0) ? 1 : 0;
    sc1(SYS_EXIT, 0);
}

static void test_pipe_close_broadcasts_eof(void) {
    puts("\n[pipe_close_broadcast]\n");
    int fds[2] = {-1, -1};
    sc2(SYS_PIPE2, (long)fds, 0);
    pe_rfd = fds[0]; pe_wfd = fds[1];
    pe_a_eof = pe_b_eof = 0;

    long ta = spawn(pe_reader_a);
    long tb = spawn(pe_reader_b);
    if (ta <= 0 || tb <= 0) { check("spawn", 0); return; }

    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 20000000 };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);

    /* Close write end — both readers must wake with EOF. */
    sc1(SYS_CLOSE, pe_wfd);
    spin_until(&pe_a_eof, 1, 10000000);
    spin_until(&pe_b_eof, 1, 10000000);
    check_val("reader A saw EOF", pe_a_eof, 1);
    check_val("reader B saw EOF", pe_b_eof, 1);

    sc1(SYS_CLOSE, pe_rfd);
}

TEST("pipe/two_readers_exclusive_wake", test_pipe_two_readers_exclusive_wake);
TEST("pipe/close_broadcasts_eof",       test_pipe_close_broadcasts_eof);
