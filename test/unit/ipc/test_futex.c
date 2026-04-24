#include "ktest.h"

/* Futex ops (Linux-compatible) */
#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_PRIVATE_FLAG  128

#define THREAD_STACK 65536

/* ── Shared state for worker threads ─────────── */

static volatile uint32_t futex_var;
static volatile int worker_result;
static volatile int worker_started;

/* Worker: wait on futex_var == expected_val */
static void wait_worker(void) {
    __sync_fetch_and_add(&worker_started, 1);
    long r = sc6(SYS_FUTEX, (long)&futex_var,
                 FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0);
    /* On wake: syscall restarts, re-checks value, returns -EAGAIN
     * because futex_wake changed the value (or the waker did).
     * Both 0 (direct wake) and -EAGAIN (value changed) are success. */
    worker_result = (int)r;
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

/* spin-wait with bounded iterations */
static void spin_until(volatile int *flag, int val, int max_iters) {
    for (int i = 0; i < max_iters && *flag != val; i++)
        __asm__ volatile("pause");
}

/* ── Tests ───────────────────────────────────── */

static void test_futex_eagain(void) {
    puts("\n[futex]\n");

    /* FUTEX_WAIT with wrong value → -EAGAIN */
    futex_var = 42;
    long r = sc6(SYS_FUTEX, (long)&futex_var,
                 FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 99, 0, 0, 0);
    check_val("WAIT wrong val = -EAGAIN", r, -EAGAIN);
}

static void test_futex_wake_none(void) {
    /* FUTEX_WAKE with no waiters → 0 */
    futex_var = 0;
    long r = sc6(SYS_FUTEX, (long)&futex_var,
                 FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0);
    check_val("WAKE no waiters = 0", r, 0);
}

static void test_futex_wait_wake(void) {
    /* Thread A waits on futex_var==1, thread B wakes it */
    futex_var = 1;
    worker_result = -999;
    worker_started = 0;

    long tid = spawn_thread(wait_worker);
    check("spawn waiter thread", tid > 0);
    if (tid <= 0) return;

    /* Wait for worker to enter futex_wait */
    spin_until(&worker_started, 1, 5000000);
    check("worker started", worker_started == 1);

    /* Small delay to let worker actually block */
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 5000000 }; /* 5ms */
    sc2(SYS_NANOSLEEP, (long)&ts, 0);

    /* Change value and wake */
    futex_var = 0;
    __sync_synchronize();
    long woken = sc6(SYS_FUTEX, (long)&futex_var,
                     FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0);
    check_ge("WAKE returns >= 1", woken, 1);

    /* Wait for worker to finish */
    spin_until(&worker_result, -EAGAIN, 5000000);
    /* Worker gets -EAGAIN (value changed on restart) or 0 (immediate wake) */
    check("worker got -EAGAIN or 0",
          worker_result == -EAGAIN || worker_result == 0);
}

static void test_futex_timeout(void) {
    /* FUTEX_WAIT with short timeout → -ETIMEDOUT */
    futex_var = 1;
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 }; /* 10ms */

    struct k_timespec before, after;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    long r = sc6(SYS_FUTEX, (long)&futex_var,
                 FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 1, (long)&ts, 0, 0);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);

    check_val("WAIT timeout = -ETIMEDOUT", r, -ETIMEDOUT);

    long elapsed_ms = (after.tv_sec - before.tv_sec) * 1000
                    + (after.tv_nsec - before.tv_nsec) / 1000000;
    check("timeout elapsed >= 5ms", elapsed_ms >= 5);
    check("timeout elapsed < 200ms", elapsed_ms < 200);
}

/* ── Phase 10.2 waitqueue-backed regressions ─────── */

/* Multiple waiters on DIFFERENT keys in the SAME hash bucket must be
 * woken independently. The old slab-based iteration filtered by (uaddr,
 * pid); the waitqueue-backed variant must do the same under the shared
 * bucket lock. Picks addresses that hit the same bucket. */
static volatile uint32_t fx_collide_a, fx_collide_b;
static volatile int fx_collide_a_done, fx_collide_b_done;

static void fx_collide_waiter_a(void) {
    long r = sc6(SYS_FUTEX, (long)&fx_collide_a,
                 FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0);
    fx_collide_a_done = (int)r;
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static void fx_collide_waiter_b(void) {
    long r = sc6(SYS_FUTEX, (long)&fx_collide_b,
                 FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0);
    fx_collide_b_done = (int)r;
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static void test_futex_bucket_key_filter(void) {
    puts("\n[futex_bucket_key_filter]\n");
    fx_collide_a = 1; fx_collide_b = 1;
    fx_collide_a_done = -999; fx_collide_b_done = -999;

    long a = spawn_thread(fx_collide_waiter_a);
    long b = spawn_thread(fx_collide_waiter_b);
    check("spawn A", a > 0);
    check("spawn B", b > 0);
    if (a <= 0 || b <= 0) return;

    /* Let both enter futex_wait */
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 20000000 };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);

    /* Wake only on key A. B must stay blocked. */
    fx_collide_a = 0;
    __sync_synchronize();
    long w = sc6(SYS_FUTEX, (long)&fx_collide_a,
                 FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0);
    check_ge("WAKE A >= 1", w, 1);

    /* Let A finish. */
    spin_until(&fx_collide_a_done, -EAGAIN, 5000000);
    check("A woke (-EAGAIN or 0)",
          fx_collide_a_done == -EAGAIN || fx_collide_a_done == 0);
    check_val("B still blocked", fx_collide_b_done, -999);

    /* Now wake B. */
    fx_collide_b = 0;
    __sync_synchronize();
    w = sc6(SYS_FUTEX, (long)&fx_collide_b,
            FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0);
    check_ge("WAKE B >= 1", w, 1);

    spin_until(&fx_collide_b_done, -EAGAIN, 5000000);
    check("B woke (-EAGAIN or 0)",
          fx_collide_b_done == -EAGAIN || fx_collide_b_done == 0);
}

/* Wake N: WAKE with val=3 on a bucket with 5 waiters must wake exactly 3. */
static volatile uint32_t fx_n_var;
static volatile int fx_n_done;

static void fx_n_waiter(void) {
    sc6(SYS_FUTEX, (long)&fx_n_var,
        FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0);
    __sync_fetch_and_add(&fx_n_done, 1);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static void test_futex_wake_n(void) {
    puts("\n[futex_wake_n]\n");
    fx_n_var = 1;
    fx_n_done = 0;

    for (int i = 0; i < 5; i++) {
        long t = spawn_thread(fx_n_waiter);
        if (t <= 0) { check("spawn", 0); return; }
    }

    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 20000000 };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);

    /* Wake 3 of 5. Value change so restarts return -EAGAIN. */
    fx_n_var = 0;
    __sync_synchronize();
    long w = sc6(SYS_FUTEX, (long)&fx_n_var,
                 FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 3, 0, 0, 0);
    check_val("WAKE 3 returns 3", w, 3);

    /* Give woken waiters a moment. */
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
    check_val("exactly 3 finished", fx_n_done, 3);

    /* Wake the rest. */
    w = sc6(SYS_FUTEX, (long)&fx_n_var,
            FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 99, 0, 0, 0);
    check_val("WAKE rest returns 2", w, 2);
    spin_until(&fx_n_done, 5, 5000000);
    check_val("all 5 finished", fx_n_done, 5);
}

TEST("futex", test_futex_eagain);
TEST("futex_wake_none", test_futex_wake_none);
TEST("futex_wait_wake", test_futex_wait_wake);
TEST("futex_timeout", test_futex_timeout);
TEST("futex_bucket_key_filter", test_futex_bucket_key_filter);
TEST("futex_wake_n", test_futex_wake_n);
