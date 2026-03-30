/* Mutex tests — PI futex from userspace
 *
 * Tests mutex_t semantics through FUTEX_LOCK_PI / FUTEX_UNLOCK_PI.
 * Uses CLONE_THREAD (same address space) since PI futex hashing is
 * pid-scoped — cross-process PI requires shared futex support.
 */
#include "ktest.h"

#define FUTEX_LOCK_PI       6
#define FUTEX_UNLOCK_PI     7
#define FUTEX_PRIVATE_FLAG  128
#define FUTEX_TID_MASK      0x3FFFFFFFU
#define FUTEX_WAITERS       0x80000000U

#define SCHED_OTHER 0
#define SCHED_FIFO  1

#define THREAD_STACK 65536

struct sched_param { int sched_priority; };

/* ── Helpers ───────────────────────────────────── */

static long pi_lock(volatile uint32_t *f) {
    return sc6(SYS_FUTEX, (long)f, FUTEX_LOCK_PI | FUTEX_PRIVATE_FLAG,
               0, 0, 0, 0);
}

static long pi_unlock(volatile uint32_t *f) {
    return sc6(SYS_FUTEX, (long)f, FUTEX_UNLOCK_PI | FUTEX_PRIVATE_FLAG,
               0, 0, 0, 0);
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

static void msleep(int ms) {
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = ms * 1000000L };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
}

static int get_priority(void) {
    struct sched_param p = {0};
    sc2(SYS_SCHED_GETPARAM, 0, (long)&p);
    return p.sched_priority;
}

static void set_scheduler(int policy, int prio) {
    struct sched_param p = { .sched_priority = prio };
    sc3(SYS_SCHED_SETSCHEDULER, 0, policy, (long)&p);
}

/* ── Shared state ──────────────────────────────── */

static volatile uint32_t mu_futex;
static volatile int mu_counter;
static volatile int mu_ready;
static volatile int mu_done;
static volatile int mu_prio_during;
static volatile int mu_prio_after;
static volatile int mu_order[4];
static volatile int mu_order_idx;

/* ── 1. PI lock/unlock roundtrip ─────────────────── */

static void test_pi_roundtrip(void) {
    puts("\n[mutex]\n");
    mu_futex = 0;

    long r = pi_lock(&mu_futex);
    check_val("PI lock = 0", r, 0);

    long tid = sc0(SYS_GETTID);
    check("futex has our TID", (int)(mu_futex & FUTEX_TID_MASK) == (int)tid);

    r = pi_unlock(&mu_futex);
    check_val("PI unlock = 0", r, 0);
    check_val("futex cleared", (long)mu_futex, 0);
}

/* ── 2. Two threads PI contention ────────────────── */

static void contend_worker(void) {
    __sync_fetch_and_add(&mu_ready, 1);
    if (pi_lock(&mu_futex) == 0) {
        __sync_fetch_and_add(&mu_counter, 1);
        pi_unlock(&mu_futex);
    }
    __sync_fetch_and_add(&mu_done, 1);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static void test_two_contend(void) {
    mu_futex = 0; mu_counter = 0; mu_ready = 0; mu_done = 0;

    /* Main thread locks */
    pi_lock(&mu_futex);

    long tid = spawn_thread(contend_worker);
    check("spawn contend worker", tid > 0);
    if (tid <= 0) return;

    spin_until(&mu_ready, 1, 5000000);
    msleep(10); /* let worker block */

    pi_unlock(&mu_futex); /* worker acquires */

    spin_until(&mu_done, 1, 10000000);
    check_val("contend: worker acquired", (long)mu_counter, 1);
}

/* ── 3. PI boost visible via sched_getparam ──────── */

static void boost_low_worker(void) {
    set_scheduler(SCHED_FIFO, 5);
    pi_lock(&mu_futex);
    __sync_fetch_and_add(&mu_ready, 1);

    /* Poll for boost */
    for (int i = 0; i < 50; i++) {
        int p = get_priority();
        if (p > 5) { mu_prio_during = p; break; }
        msleep(5);
    }
    if (mu_prio_during == 0)
        mu_prio_during = get_priority();

    pi_unlock(&mu_futex);
    mu_prio_after = get_priority();
    __sync_fetch_and_add(&mu_done, 1);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static void test_pi_boost(void) {
    mu_futex = 0; mu_ready = 0; mu_done = 0;
    mu_prio_during = 0; mu_prio_after = 0;

    long tid = spawn_thread(boost_low_worker);
    check("spawn boost worker", tid > 0);
    if (tid <= 0) return;

    spin_until(&mu_ready, 1, 10000000);
    msleep(5);

    set_scheduler(SCHED_FIFO, 20);
    pi_lock(&mu_futex);
    pi_unlock(&mu_futex);

    spin_until(&mu_done, 1, 10000000);
    check("PI boost: holder prio raised", mu_prio_during >= 20);
    set_scheduler(SCHED_OTHER, 0);
}

/* ── 4. PI deboost: priority restored after unlock ── */

static void test_pi_deboost(void) {
    mu_futex = 0; mu_ready = 0; mu_done = 0;
    mu_prio_during = 0; mu_prio_after = -1;

    long tid = spawn_thread(boost_low_worker); /* reuse: sets prio 5, locks, polls, unlocks */
    check("spawn deboost worker", tid > 0);
    if (tid <= 0) return;

    spin_until(&mu_ready, 1, 10000000);
    msleep(5);

    set_scheduler(SCHED_FIFO, 20);
    pi_lock(&mu_futex);
    pi_unlock(&mu_futex);

    spin_until(&mu_done, 1, 10000000);
    check_val("PI deboost: prio back to 5", (long)mu_prio_after, 5);
    set_scheduler(SCHED_OTHER, 0);
}

/* ── 5. Trylock via CAS on futex word ────────────── */

static void test_trylock(void) {
    volatile uint32_t futex = 0;
    long tid = sc0(SYS_GETTID);

    /* CAS 0 → tid: acquire */
    uint32_t old = __sync_val_compare_and_swap(&futex, 0, (uint32_t)tid);
    check_val("trylock CAS succeed", (long)old, 0);

    /* CAS 0 → tid again: should fail */
    old = __sync_val_compare_and_swap(&futex, 0, (uint32_t)tid);
    check("trylock CAS busy", old != 0);

    futex = 0;
    __sync_synchronize();
}

/* ── 6. Stress: 4 threads × 50 PI lock cycles ───── */

static void stress_worker(void) {
    for (int i = 0; i < 50; i++) {
        if (pi_lock(&mu_futex) == 0) {
            __sync_fetch_and_add(&mu_counter, 1);
            pi_unlock(&mu_futex);
        }
    }
    __sync_fetch_and_add(&mu_done, 1);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static void test_stress(void) {
    mu_futex = 0; mu_counter = 0; mu_done = 0;

    for (int i = 0; i < 4; i++) {
        long tid = spawn_thread(stress_worker);
        if (tid <= 0) { fail("spawn stress worker", 0); return; }
    }

    for (int i = 0; i < 50000000 && mu_done < 4; i++)
        __asm__ volatile("pause");

    check_val("stress: all done", (long)mu_done, 4);
    check_val("stress: 200 acquisitions", (long)mu_counter, 200);
}

/* ── 7. Lock ordering: highest prio acquires first ── */

static void order_worker_10(void) {
    set_scheduler(SCHED_FIFO, 10);
    __sync_fetch_and_add(&mu_ready, 1);
    pi_lock(&mu_futex);
    int idx = __sync_fetch_and_add((volatile int *)&mu_order_idx, 1);
    mu_order[idx] = 10;
    pi_unlock(&mu_futex);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static void order_worker_20(void) {
    set_scheduler(SCHED_FIFO, 20);
    __sync_fetch_and_add(&mu_ready, 1);
    pi_lock(&mu_futex);
    int idx = __sync_fetch_and_add((volatile int *)&mu_order_idx, 1);
    mu_order[idx] = 20;
    pi_unlock(&mu_futex);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static void test_lock_ordering(void) {
    mu_futex = 0; mu_ready = 0; mu_order_idx = 0;
    for (int i = 0; i < 4; i++) mu_order[i] = 0;

    pi_lock(&mu_futex);

    spawn_thread(order_worker_10);
    spawn_thread(order_worker_20);

    spin_until(&mu_ready, 2, 10000000);
    msleep(10); /* let both block */

    pi_unlock(&mu_futex);

    for (int i = 0; i < 50000000 && mu_order_idx < 2; i++)
        __asm__ volatile("pause");

    check_val("ordering: both completed", (long)mu_order_idx, 2);
    int has10 = 0, has20 = 0;
    for (int i = 0; i < 2; i++) {
        if (mu_order[i] == 10) has10 = 1;
        if (mu_order[i] == 20) has20 = 1;
    }
    check("ordering: both prios present", has10 && has20);
    set_scheduler(SCHED_OTHER, 0);
}

/* ── 8. No contention fast path ──────────────────── */

static void test_fast_path(void) {
    mu_futex = 0;

    for (int i = 0; i < 100; i++) {
        long r = pi_lock(&mu_futex);
        if (r != 0) { fail("fast_path lock", 0); return; }
        r = pi_unlock(&mu_futex);
        if (r != 0) { fail("fast_path unlock", 0); return; }
    }
    pass("fast path 100 cycles");
}

/* ── 9. Double unlock → -EPERM ───────────────────── */

static void test_double_unlock(void) {
    mu_futex = 0;
    pi_lock(&mu_futex);
    pi_unlock(&mu_futex);

    long r = pi_unlock(&mu_futex);
    check_val("double unlock = -EPERM", r, -EPERM);
}

/* ── 10. Lock held across sleep ──────────────────── */

static void sleep_holder_worker(void) {
    pi_lock(&mu_futex);
    __sync_fetch_and_add(&mu_ready, 1);
    msleep(5);
    mu_counter = 99;
    pi_unlock(&mu_futex);
    __sync_fetch_and_add(&mu_done, 1);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static void test_lock_across_sleep(void) {
    mu_futex = 0; mu_counter = 0; mu_ready = 0; mu_done = 0;

    long tid = spawn_thread(sleep_holder_worker);
    check("spawn sleep holder", tid > 0);
    if (tid <= 0) return;

    spin_until(&mu_ready, 1, 5000000);
    msleep(1);

    pi_lock(&mu_futex);
    int val = mu_counter;
    pi_unlock(&mu_futex);

    spin_until(&mu_done, 1, 10000000);
    check_val("lock across sleep: counter = 99", (long)val, 99);
}

/* ── Register ──────────────────────────────────── */

TEST("mutex", test_pi_roundtrip);
TEST("mutex", test_two_contend);
TEST("mutex", test_pi_boost);
TEST("mutex", test_pi_deboost);
TEST("mutex", test_trylock);
TEST("mutex", test_stress);
TEST("mutex", test_lock_ordering);
TEST("mutex", test_fast_path);
TEST("mutex", test_double_unlock);
TEST("mutex", test_lock_across_sleep);
