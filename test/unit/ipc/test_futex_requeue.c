#include "ktest.h"

#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_REQUEUE       3
#define FUTEX_CMP_REQUEUE   4
#define FUTEX_PRIVATE_FLAG  128

#define THREAD_STACK 65536
#define FP (FUTEX_PRIVATE_FLAG)

/* ── Shared state ───────────────────────────────── */

static volatile uint32_t addr1, addr2;
static volatile int worker_started;
static volatile int worker_woken;

static void requeue_waiter(void) {
    __sync_fetch_and_add(&worker_started, 1);
    sc6(SYS_FUTEX, (long)&addr1, FUTEX_WAIT | FP, 1, 0, 0, 0);
    __sync_fetch_and_add(&worker_woken, 1);
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

/* ── Tests ──────────────────────────────────────── */

static void test_futex_requeue_basic(void) {
    puts("\n[futex_requeue]\n");

    /* Thread waits on addr1, requeue to addr2, wake on addr2 */
    addr1 = 1;
    addr2 = 0;
    worker_started = 0;
    worker_woken = 0;

    long tid = spawn_thread(requeue_waiter);
    check("spawn waiter", tid > 0);
    if (tid <= 0) return;

    spin_until(&worker_started, 1, 5000000);
    delay_ms(5);

    /* REQUEUE: wake 0 on addr1, move 1 to addr2
     * futex(addr1, REQUEUE, val=0, val2=1, addr2, val3=0) */
    long r = sc6(SYS_FUTEX, (long)&addr1, FUTEX_REQUEUE | FP,
                 0, 1, (long)&addr2, 0);
    check_val("REQUEUE moved 1", r, 1);

    /* Wake the requeued waiter on addr2 */
    addr1 = 0;
    __sync_synchronize();
    long w = sc6(SYS_FUTEX, (long)&addr2, FUTEX_WAKE | FP, 1, 0, 0, 0);
    check_ge("WAKE addr2 >= 1", w, 1);

    spin_until(&worker_woken, 1, 5000000);
    check_val("worker woken", worker_woken, 1);
}

static void test_futex_cmp_requeue(void) {
    /* CMP_REQUEUE with correct val3 */
    addr1 = 1;
    addr2 = 0;
    worker_started = 0;
    worker_woken = 0;

    long tid = spawn_thread(requeue_waiter);
    check("spawn waiter", tid > 0);
    if (tid <= 0) return;

    spin_until(&worker_started, 1, 5000000);
    delay_ms(5);

    /* CMP_REQUEUE: wake 0, requeue 1, check *addr1 == 1 (val3)
     * futex(addr1, CMP_REQUEUE, val=0, val2=1, addr2, val3=1) */
    long r = sc6(SYS_FUTEX, (long)&addr1, FUTEX_CMP_REQUEUE | FP,
                 0, 1, (long)&addr2, 1);
    check_val("CMP_REQUEUE moved 1", r, 1);

    addr1 = 0;
    __sync_synchronize();
    long w = sc6(SYS_FUTEX, (long)&addr2, FUTEX_WAKE | FP, 1, 0, 0, 0);
    check_ge("WAKE addr2 >= 1", w, 1);

    spin_until(&worker_woken, 1, 5000000);
    check_val("worker woken", worker_woken, 1);
}

static void test_futex_cmp_requeue_fail(void) {
    /* CMP_REQUEUE with wrong val3 → -EAGAIN */
    addr1 = 42;
    long r = sc6(SYS_FUTEX, (long)&addr1, FUTEX_CMP_REQUEUE | FP,
                 0, 1, (long)&addr2, 99);
    check_val("CMP_REQUEUE wrong val3 = -EAGAIN", r, -EAGAIN);
}

/* ── Regression: stale-bucket race after REQUEUE ────
 *
 * pthread_cond-smasher reproduced this: many threads cond_wait on cond.seq,
 * cond_broadcast does FUTEX_REQUEUE moving them to mutex.bucket, then
 * mutex_unlock wakes one. The wake sets state RUNNABLE before the woken
 * thread observes the requeue; on resume the thread's prepare_to_wait /
 * finish_wait used the *original* bucket pointer cached on its stack —
 * which corrupted the new bucket's doubly-linked list (tail->next at
 * NULL+0x18 → KERNEL PF cr2=0x18 in futex_requeue Phase-2 INSERT).
 *
 * Reliable repro: spawn N waiters on addr1, REQUEUE all to addr2, WAKE
 * each individually, then run a SECOND REQUEUE cycle on a fresh waiter
 * targeting the same bucket. Without the fw->bucket update the second
 * cycle's tail is the corrupt entry left behind. */
#define SMASH_WORKERS 4
static volatile int smash_woken;

static void smash_worker(void) {
    __sync_fetch_and_add(&worker_started, 1);
    /* Block on addr1 == 1; expected wake path: REQUEUE -> addr2 then WAKE */
    sc6(SYS_FUTEX, (long)&addr1, FUTEX_WAIT | FP, 1, 0, 0, 0);
    __sync_fetch_and_add(&smash_woken, 1);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static void test_futex_requeue_smash(void) {
    addr1 = 1;
    addr2 = 0;
    worker_started = 0;
    smash_woken = 0;

    for (int i = 0; i < SMASH_WORKERS; i++) {
        long tid = spawn_thread(smash_worker);
        check("spawn smash worker", tid > 0);
        if (tid <= 0) return;
    }
    spin_until(&worker_started, SMASH_WORKERS, 10000000);
    check_val("all started", worker_started, SMASH_WORKERS);
    delay_ms(10);

    /* Phase 1: requeue all from addr1 to addr2 */
    long moved = sc6(SYS_FUTEX, (long)&addr1, FUTEX_REQUEUE | FP,
                     0, SMASH_WORKERS, (long)&addr2, 0);
    check_val("REQUEUE moved all", moved, SMASH_WORKERS);

    /* Phase 2: wake one at a time on addr2. With the stale-bucket bug,
     * the first wake sets state via the moved entry; the woken thread's
     * finish_wait runs against addr1's bucket (stale), corrupting addr2's
     * list — the next iteration's WAKE then dereferences a NULL prev. */
    addr1 = 0;
    __sync_synchronize();
    int w_total = 0;
    for (int i = 0; i < SMASH_WORKERS; i++) {
        long w = sc6(SYS_FUTEX, (long)&addr2, FUTEX_WAKE | FP, 1, 0, 0, 0);
        check("WAKE >= 0", w >= 0);
        if (w > 0) w_total += (int)w;
        delay_ms(2);
    }
    check_ge("woke at least N", w_total, SMASH_WORKERS);

    spin_until(&smash_woken, SMASH_WORKERS, 20000000);
    check_val("all workers exited", smash_woken, SMASH_WORKERS);
}

TEST("futex_requeue_basic", test_futex_requeue_basic);
TEST("futex_cmp_requeue", test_futex_cmp_requeue);
TEST("futex_cmp_requeue_fail", test_futex_cmp_requeue_fail);
TEST("futex_requeue_smash", test_futex_requeue_smash);
