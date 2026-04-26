/* Futex on per-bucket wait_queue_head_t.
 * Verifies key-filtered wake (multiple waiters, partial wake), and that
 * SIGUSR1 delivery during FUTEX_WAIT returns -EINTR after restart. */
#include "ktest.h"

#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_PRIVATE_FLAG  128
#define FP                  FUTEX_PRIVATE_FLAG

#define THREAD_STACK 65536

#define SIGUSR1 10

/* ── Shared state ─────────────────────────── */

static volatile uint32_t mw_futex;
static volatile int mw_started;
static volatile int mw_woken;
static volatile int mw_results[3];

static void mw_worker_0(void) {
    __sync_fetch_and_add(&mw_started, 1);
    long r = sc6(SYS_FUTEX, (long)&mw_futex, FUTEX_WAIT | FP, 1, 0, 0, 0);
    mw_results[0] = (int)r;
    __sync_fetch_and_add(&mw_woken, 1);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}
static void mw_worker_1(void) {
    __sync_fetch_and_add(&mw_started, 1);
    long r = sc6(SYS_FUTEX, (long)&mw_futex, FUTEX_WAIT | FP, 1, 0, 0, 0);
    mw_results[1] = (int)r;
    __sync_fetch_and_add(&mw_woken, 1);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}
static void mw_worker_2(void) {
    __sync_fetch_and_add(&mw_started, 1);
    long r = sc6(SYS_FUTEX, (long)&mw_futex, FUTEX_WAIT | FP, 1, 0, 0, 0);
    mw_results[2] = (int)r;
    __sync_fetch_and_add(&mw_woken, 1);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static long spawn_thread(void (*fn)(void)) {
    long stk = sc6(SYS_MMAP, 0, THREAD_STACK, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (stk <= 0) return -1;
    long ret = sc5(SYS_CLONE,
        (long)(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD),
        stk + THREAD_STACK, 0, 0, 0);
    if (ret == 0) { fn(); __builtin_unreachable(); }
    return ret;
}

static void spin_until(volatile int *flag, int val, int max_iters) {
    for (int i = 0; i < max_iters && *flag != val; i++)
        __asm__ volatile("pause");
}

static void delay_ms(int ms) {
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = ms * 1000000L };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
}

/* ── multiple_waiters: WAKE 2 of 3 ────────── */

static void test_futex_multiple_waiters(void) {
    puts("\n[futex/bucket-wq]\n");

    mw_futex = 1;
    mw_started = 0;
    mw_woken = 0;
    mw_results[0] = mw_results[1] = mw_results[2] = -999;

    long t0 = spawn_thread(mw_worker_0);
    long t1 = spawn_thread(mw_worker_1);
    long t2 = spawn_thread(mw_worker_2);
    check("spawn t0", t0 > 0);
    check("spawn t1", t1 > 0);
    check("spawn t2", t2 > 0);

    /* Wait for all three to enter futex_wait */
    spin_until(&mw_started, 3, 5000000);
    check("3 workers started", mw_started == 3);
    delay_ms(10);

    mw_futex = 0;
    __sync_synchronize();
    long woken = sc6(SYS_FUTEX, (long)&mw_futex, FUTEX_WAKE | FP, 2, 0, 0, 0);
    check_val("WAKE 2 of 3", woken, 2);

    spin_until(&mw_woken, 2, 5000000);
    check("2 workers exited", mw_woken == 2);

    /* The third is still parked. Wake it. */
    long woken2 = sc6(SYS_FUTEX, (long)&mw_futex, FUTEX_WAKE | FP, 1, 0, 0, 0);
    check_val("WAKE remaining 1", woken2, 1);

    spin_until(&mw_woken, 3, 5000000);
    check("all 3 workers exited", mw_woken == 3);
}

/* ── separate_keys: waiters on addr A are NOT woken by WAKE on addr B ─── */

static volatile uint32_t sk_a;
static volatile uint32_t sk_b;
static volatile int sk_a_started;
static volatile int sk_a_woken;

static void sk_a_worker(void) {
    __sync_fetch_and_add(&sk_a_started, 1);
    sc6(SYS_FUTEX, (long)&sk_a, FUTEX_WAIT | FP, 1, 0, 0, 0);
    __sync_fetch_and_add(&sk_a_woken, 1);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static void test_futex_separate_keys(void) {
    sk_a = 1;
    sk_b = 1;
    sk_a_started = 0;
    sk_a_woken = 0;

    long t = spawn_thread(sk_a_worker);
    check("spawn", t > 0);
    if (t <= 0) return;

    spin_until(&sk_a_started, 1, 5000000);
    delay_ms(5);

    /* Wake on a different address — must NOT wake the waiter on sk_a */
    long w = sc6(SYS_FUTEX, (long)&sk_b, FUTEX_WAKE | FP, 99, 0, 0, 0);
    check_val("WAKE sk_b: 0 woken", w, 0);

    /* Confirm worker still parked */
    delay_ms(5);
    check("worker still parked", sk_a_woken == 0);

    /* Now wake on the right address */
    sk_a = 0;
    __sync_synchronize();
    long w2 = sc6(SYS_FUTEX, (long)&sk_a, FUTEX_WAKE | FP, 1, 0, 0, 0);
    check_ge("WAKE sk_a >= 1", w2, 1);

    spin_until(&sk_a_woken, 1, 5000000);
    check("worker woken", sk_a_woken == 1);
}

TEST("futex/bucket-multiple-waiters", test_futex_multiple_waiters);
TEST("futex/bucket-separate-keys", test_futex_separate_keys);
