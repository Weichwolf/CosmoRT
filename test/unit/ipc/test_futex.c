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

/* Cross-process FUTEX_WAIT (no PRIVATE flag) on MAP_SHARED page.
 * Mirrors LTP's tst_checkpoint_wait/wake pattern: parent waits, child wakes
 * a page the child has never read. Without GUP-style demand-fault inside
 * futex_key, child's PA-lookup returns 0 and WAKE goes into a private-key
 * bucket the parent's waiter is not in. */
static void test_futex_shared_cross_process(void) {
    puts("\n[futex/shared-cross-process]\n");

    long page = sc6(SYS_MMAP, 0, 4096, PROT_RW,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check("mmap shared", page > 0);
    if (page <= 0) return;

    volatile uint32_t *fx = (volatile uint32_t *)page;
    *fx = 0;

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) { sc2(SYS_MUNMAP, page, 4096); return; }

    if (pid == 0) {
        struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 };
        sc2(SYS_NANOSLEEP, (long)&ts, 0);
        long woken = sc6(SYS_FUTEX, (long)fx, FUTEX_WAKE, 1, 0, 0, 0);
        sc1(SYS_EXIT, woken == 1 ? 0 : 1);
        __builtin_unreachable();
    }

    struct k_timespec to = { .tv_sec = 2, .tv_nsec = 0 };
    long r = sc6(SYS_FUTEX, (long)fx, FUTEX_WAIT, 0, (long)&to, 0, 0);
    check("parent FUTEX_WAIT returns 0 or -EINTR", r == 0 || r == -EINTR);

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited cleanly", (wstatus & 0x7F) == 0);
    check_val("child WAKE saw exactly 1 waiter", (wstatus >> 8) & 0xFF, 0);

    sc2(SYS_MUNMAP, page, 4096);
}

TEST("futex", test_futex_eagain);
TEST("futex_wake_none", test_futex_wake_none);
TEST("futex_wait_wake", test_futex_wait_wake);
TEST("futex_timeout", test_futex_timeout);
TEST("futex/shared-cross-process", test_futex_shared_cross_process);
