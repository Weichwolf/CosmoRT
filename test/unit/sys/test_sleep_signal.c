/* Tests fuer signal-interrupted sleep syscalls.
 * clock_nanosleep/nanosleep/pause muessen per -EINTR zurueckkehren
 * wenn Signal blocked-Thread trifft. rem-Zeit muss > 0 und < req sein. */
#include "ktest.h"

struct ksigaction_t {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

__attribute__((naked)) static void sr(void) {
    __asm__ volatile("mov $15, %%rax\n" "syscall\n" ::: "memory");
}

static volatile int h_sig;
static void h(int sig) { h_sig = sig; }

static void install_handler(int sig) {
    struct ksigaction_t sa = {
        .handler = (void *)h,
        .flags = SA_RESTORER,
        .restorer = (void *)sr,
        .mask = 0,
    };
    sc4(SYS_RT_SIGACTION, sig, (long)&sa, 0, 8);
}

static long mono_ms(void) {
    struct { long sec, nsec; } ts;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&ts);
    return ts.sec * 1000 + ts.nsec / 1000000;
}

/* Child-Prozess schickt Signal an parent nach delay_ms. */
static long spawn_signaler(long target_pid, int sig, int delay_ms) {
    long pid = sc0(SYS_FORK);
    if (pid != 0) return pid;

    struct { long sec, nsec; } ts;
    ts.sec = delay_ms / 1000;
    ts.nsec = (delay_ms % 1000) * 1000000;
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
    sc2(SYS_KILL, target_pid, sig);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

/* LTP-style: fork child that sends `count` signals with `us` interval. */
static long spawn_signaler_loop(long target_pid, int sig, int count, int us) {
    long pid = sc0(SYS_FORK);
    if (pid != 0) return pid;

    struct { long sec, nsec; } ts = { .sec = us / 1000000, .nsec = (us % 1000000) * 1000 };
    for (int i = 0; i < count; i++) {
        sc2(SYS_NANOSLEEP, (long)&ts, 0);
        sc2(SYS_KILL, target_pid, sig);
    }
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

/* ── Test 1: clock_nanosleep (relative) interrupted by SIGINT ── */
static void test_clock_nanosleep_eintr(void) {
    puts("\n[sleep-signal: clock_nanosleep EINTR]\n");

    install_handler(SIGINT);
    h_sig = 0;

    long self = sc0(SYS_GETPID);
    long child = spawn_signaler(self, SIGINT, 200);
    check("signaler forked", child > 0);

    struct { long sec, nsec; } req = { .sec = 5, .nsec = 0 };
    struct { long sec, nsec; } rem = { .sec = -1, .nsec = -1 };

    long t0 = mono_ms();
    long r = sc4(SYS_CLOCK_NANOSLEEP, CLOCK_MONOTONIC, 0, (long)&req, (long)&rem);
    long elapsed = mono_ms() - t0;

    check_val("clock_nanosleep returns -EINTR", r, -EINTR);
    check("returns within 2s", elapsed < 2000);
    check_val("handler ran with SIGINT", h_sig, SIGINT);
    check("rem.sec > 0", rem.sec > 0);
    check("rem.sec < 5", rem.sec < 5);

    int st = 0;
    sc4(SYS_WAIT4, child, (long)&st, 0, 0);
}
TEST("sleep-signal-clock_nanosleep", test_clock_nanosleep_eintr);

/* ── Test 2: nanosleep (relative) interrupted by SIGUSR1 ── */
static void test_nanosleep_eintr(void) {
    puts("\n[sleep-signal: nanosleep EINTR]\n");

    install_handler(SIGUSR1);
    h_sig = 0;

    long self = sc0(SYS_GETPID);
    long child = spawn_signaler(self, SIGUSR1, 150);
    check("signaler forked", child > 0);

    struct { long sec, nsec; } req = { .sec = 3, .nsec = 0 };
    struct { long sec, nsec; } rem = { .sec = -1, .nsec = -1 };

    long t0 = mono_ms();
    long r = sc2(SYS_NANOSLEEP, (long)&req, (long)&rem);
    long elapsed = mono_ms() - t0;

    check_val("nanosleep returns -EINTR", r, -EINTR);
    check("returns within 1500ms", elapsed < 1500);
    check_val("handler ran with SIGUSR1", h_sig, SIGUSR1);
    check("rem.sec > 0 or rem.nsec > 0", rem.sec > 0 || rem.nsec > 0);

    int st = 0;
    sc4(SYS_WAIT4, child, (long)&st, 0, 0);
}
TEST("sleep-signal-nanosleep", test_nanosleep_eintr);

/* ── Test 3: 500x 1ms sleeps — must complete in reasonable time ── */
static void test_repeated_short_sleep(void) {
    puts("\n[sleep-signal: 500x 1ms sleep]\n");

    long t0 = mono_ms();
    for (int i = 0; i < 500; i++) {
        struct { long sec, nsec; } req = { .sec = 0, .nsec = 1000000 };
        sc2(SYS_NANOSLEEP, (long)&req, 0);
    }
    long elapsed = mono_ms() - t0;

    puts("  500x 1ms elapsed="); put_int(elapsed); puts("ms\n");
    /* Linux: 500ms-1s on idle system. We allow 4s to pass. */
    check("500x 1ms sleep < 4s", elapsed < 4000);
}
TEST("sleep-signal-500x1ms", test_repeated_short_sleep);

/* ── Test 4: pause interrupted by signal ── */
static void test_pause_eintr(void) {
    puts("\n[sleep-signal: pause EINTR]\n");

    install_handler(SIGUSR1);
    h_sig = 0;

    long self = sc0(SYS_GETPID);
    long child = spawn_signaler(self, SIGUSR1, 100);
    check("signaler forked", child > 0);

    long t0 = mono_ms();
    long r = sc0(SYS_PAUSE);
    long elapsed = mono_ms() - t0;

    check_val("pause returns -EINTR", r, -EINTR);
    check("pause returns within 1s", elapsed < 1000);
    check_val("handler ran with SIGUSR1", h_sig, SIGUSR1);

    int st = 0;
    sc4(SYS_WAIT4, child, (long)&st, 0, 0);
}
TEST("sleep-signal-pause", test_pause_eintr);

/* ── Test 5: LTP-style rapid SIGINT storm (40x SIGINT at 500us intervals) ── */
static void test_clock_nanosleep_sigstorm(void) {
    puts("\n[sleep-signal: LTP-style 40x SIGINT @500us]\n");

    install_handler(SIGINT);
    h_sig = 0;

    long self = sc0(SYS_GETPID);
    long child = spawn_signaler_loop(self, SIGINT, 40, 500);
    check("signaler forked", child > 0);

    struct { long sec, nsec; } req = { .sec = 10, .nsec = 0 };
    struct { long sec, nsec; } rem = { .sec = -1, .nsec = -1 };

    long t0 = mono_ms();
    long r = sc4(SYS_CLOCK_NANOSLEEP, CLOCK_REALTIME, 0, (long)&req, (long)&rem);
    long elapsed = mono_ms() - t0;

    puts("  elapsed="); put_int(elapsed); puts("ms\n");
    check_val("clock_nanosleep returns -EINTR", r, -EINTR);
    check("returns within 500ms", elapsed < 500);
    check_val("handler ran with SIGINT", h_sig, SIGINT);
    check("rem.sec > 0", rem.sec > 0);

    int st = 0;
    sc4(SYS_WAIT4, child, (long)&st, 0, 0);
}
TEST("sleep-signal-sigstorm", test_clock_nanosleep_sigstorm);

/* ── Test 6: clock_nanosleep TIMER_ABSTIME interrupted ── */
static void test_clock_nanosleep_abs_eintr(void) {
    puts("\n[sleep-signal: clock_nanosleep ABS EINTR]\n");

    install_handler(SIGINT);
    h_sig = 0;

    long self = sc0(SYS_GETPID);
    long child = spawn_signaler(self, SIGINT, 200);
    check("signaler forked", child > 0);

    struct { long sec, nsec; } now;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&now);
    struct { long sec, nsec; } abs = { .sec = now.sec + 5, .nsec = now.nsec };

    long t0 = mono_ms();
    long r = sc4(SYS_CLOCK_NANOSLEEP, CLOCK_MONOTONIC, TIMER_ABSTIME,
                 (long)&abs, 0);
    long elapsed = mono_ms() - t0;

    check_val("clock_nanosleep ABS returns -EINTR", r, -EINTR);
    check("ABS returns within 1500ms", elapsed < 1500);
    check_val("handler ran with SIGINT", h_sig, SIGINT);

    int st = 0;
    sc4(SYS_WAIT4, child, (long)&st, 0, 0);
}
TEST("sleep-signal-clock_nanosleep-abs", test_clock_nanosleep_abs_eintr);
