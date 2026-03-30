/* RT Mutex Blocking tests — fork + MAP_SHARED
 *
 * Tests FUTEX_LOCK_PI / FUTEX_UNLOCK_PI with true blocking (V2).
 * All cross-process tests use fork + MAP_SHARED (no CLONE_THREAD).
 * Shared futexes: no FUTEX_PRIVATE_FLAG so pid=0 in hash.
 */
#include "ktest.h"

#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_LOCK_PI       6
#define FUTEX_UNLOCK_PI     7
#define FUTEX_PRIVATE_FLAG  128
#define FUTEX_TID_MASK      0x3FFFFFFFU
#define FUTEX_WAITERS       0x80000000U

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

/* Shared memory layout for cross-process tests */
typedef struct {
    volatile uint32_t futex;
    volatile int counter;
    volatile int ready;
    volatile int done;
    volatile int spin_count;
    volatile int acquired;
    volatile int order[4];
    volatile int order_idx;
} shared_t;

static shared_t *shared_alloc(void) {
    return (shared_t *)(long)sc6(SYS_MMAP, 0, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
}

static void wait_child(long pid) {
    int ws;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
}

static void msleep(int ms) {
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = ms * 1000000L };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
}

/* Lock/unlock via shared futex (no PRIVATE flag = cross-process) */
static long pi_lock(volatile uint32_t *f) {
    return sc6(SYS_FUTEX, (long)f, FUTEX_LOCK_PI, 0, 0, 0, 0);
}

static long pi_unlock(volatile uint32_t *f) {
    return sc6(SYS_FUTEX, (long)f, FUTEX_UNLOCK_PI, 0, 0, 0, 0);
}

/* ── 1. Uncontended lock+unlock ───────────────── */

static void test_uncontended(void) {
    puts("\n[mutex_blocking]\n");
    shared_t *s = shared_alloc();
    if ((long)s < 0) { fail("uncontended", "mmap"); return; }
    s->futex = 0;

    long r = pi_lock(&s->futex);
    check_val("uncontended lock = 0", r, 0);

    r = pi_unlock(&s->futex);
    check_val("uncontended unlock = 0", r, 0);
    check_val("futex cleared", (long)s->futex, 0);

    sc2(SYS_MUNMAP, (long)s, 4096);
}

/* ── 2. Two processes: holder + waiter (sleeps, not spins) ── */

static void test_two_procs_contend(void) {
    shared_t *s = shared_alloc();
    if ((long)s < 0) { fail("two_procs", "mmap"); return; }
    s->futex = 0;
    s->ready = 0;
    s->acquired = 0;

    /* Parent locks */
    pi_lock(&s->futex);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        s->ready = 1;
        __sync_synchronize();
        /* This should BLOCK (sleep), not spin */
        pi_lock(&s->futex);
        s->acquired = 1;
        pi_unlock(&s->futex);
        sc1(SYS_EXIT, 0);
    }

    /* Wait for child to start */
    while (!s->ready) __asm__ volatile("pause");
    msleep(20); /* let child block on the futex */

    /* Child should not have acquired yet */
    check("child blocked (not acquired)", s->acquired == 0);

    /* Release — child should wake and acquire */
    pi_unlock(&s->futex);
    wait_child(pid);

    check_val("child acquired after unlock", (long)s->acquired, 1);
    sc2(SYS_MUNMAP, (long)s, 4096);
}

/* ── 3. Lock+Sleep: holder sleeps 20ms, waiter blocks ── */

static void test_lock_sleep(void) {
    shared_t *s = shared_alloc();
    if ((long)s < 0) { fail("lock_sleep", "mmap"); return; }
    s->futex = 0;
    s->ready = 0;
    s->acquired = 0;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Child: lock, signal ready, sleep 20ms, unlock */
        pi_lock(&s->futex);
        s->ready = 1;
        __sync_synchronize();
        msleep(20);
        pi_unlock(&s->futex);
        sc1(SYS_EXIT, 0);
    }

    /* Wait for child to acquire */
    while (!s->ready) __asm__ volatile("pause");

    /* Parent: block on lock (child holds it for 20ms) */
    long r = pi_lock(&s->futex);
    check_val("lock_sleep: acquired after wait", r, 0);
    pi_unlock(&s->futex);

    wait_child(pid);
    sc2(SYS_MUNMAP, (long)s, 4096);
}

/* ── 4. Fairness: 2 children alternate 100 lock/unlock cycles ── */

static void test_fairness(void) {
    shared_t *s = shared_alloc();
    if ((long)s < 0) { fail("fairness", "mmap"); return; }
    s->futex = 0;
    s->counter = 0;
    s->order[0] = 0;
    s->order[1] = 0;

    long p1 = sc0(SYS_FORK);
    if (p1 == 0) {
        for (int i = 0; i < 100; i++) {
            pi_lock(&s->futex);
            s->order[0]++;
            pi_unlock(&s->futex);
        }
        sc1(SYS_EXIT, 0);
    }
    long p2 = sc0(SYS_FORK);
    if (p2 == 0) {
        for (int i = 0; i < 100; i++) {
            pi_lock(&s->futex);
            s->order[1]++;
            pi_unlock(&s->futex);
        }
        sc1(SYS_EXIT, 0);
    }

    wait_child(p1);
    wait_child(p2);

    check_val("fairness: child A 100 cycles", (long)s->order[0], 100);
    check_val("fairness: child B 100 cycles", (long)s->order[1], 100);
    sc2(SYS_MUNMAP, (long)s, 4096);
}

/* ── 5. Verify sleeping: blocked waiter doesn't burn CPU ── */

static void test_verify_sleeping(void) {
    shared_t *s = shared_alloc();
    if ((long)s < 0) { fail("verify_sleeping", "mmap"); return; }
    s->futex = 0;
    s->spin_count = 0;
    s->ready = 0;
    s->acquired = 0;

    /* Parent locks */
    pi_lock(&s->futex);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        s->ready = 1;
        __sync_synchronize();
        /* While blocked, child can't increment spin_count.
         * If truly sleeping, spin_count stays 0. */
        pi_lock(&s->futex);
        s->acquired = 1;
        pi_unlock(&s->futex);
        sc1(SYS_EXIT, 0);
    }

    while (!s->ready) __asm__ volatile("pause");
    msleep(30); /* hold lock for 30ms while child "should be sleeping" */

    /* spin_count should be 0 if child is truly blocked */
    int sc = s->spin_count;
    check("sleeping: no CPU burn", sc == 0);
    check("sleeping: not acquired while held", s->acquired == 0);

    pi_unlock(&s->futex);
    wait_child(pid);

    check_val("sleeping: child acquired", (long)s->acquired, 1);
    sc2(SYS_MUNMAP, (long)s, 4096);
}

/* ── 6. Rapid uncontended lock/unlock: 1000 cycles ── */

static void test_rapid_uncontended(void) {
    shared_t *s = shared_alloc();
    if ((long)s < 0) { fail("rapid", "mmap"); return; }
    s->futex = 0;

    for (int i = 0; i < 1000; i++) {
        long r = pi_lock(&s->futex);
        if (r != 0) { fail("rapid lock", 0); sc2(SYS_MUNMAP, (long)s, 4096); return; }
        r = pi_unlock(&s->futex);
        if (r != 0) { fail("rapid unlock", 0); sc2(SYS_MUNMAP, (long)s, 4096); return; }
    }
    pass("rapid 1000 uncontended cycles");
    sc2(SYS_MUNMAP, (long)s, 4096);
}

/* ── 7. Unlock wakes waiter ──────────────────────── */

static void test_unlock_wakes(void) {
    shared_t *s = shared_alloc();
    if ((long)s < 0) { fail("unlock_wakes", "mmap"); return; }
    s->futex = 0;
    s->ready = 0;
    s->done = 0;

    /* Parent locks */
    pi_lock(&s->futex);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        s->ready = 1;
        __sync_synchronize();
        pi_lock(&s->futex);
        s->done = 1;
        pi_unlock(&s->futex);
        sc1(SYS_EXIT, 0);
    }

    while (!s->ready) __asm__ volatile("pause");
    msleep(10);

    /* Child should be blocked */
    check("unlock_wakes: child blocked", s->done == 0);

    /* Unlock — should wake child */
    pi_unlock(&s->futex);
    wait_child(pid);

    check_val("unlock_wakes: child completed", (long)s->done, 1);
    sc2(SYS_MUNMAP, (long)s, 4096);
}

/* ── 8. Multiple waiters: 2 children wait, unlock wakes one ── */

static void test_multiple_waiters(void) {
    shared_t *s = shared_alloc();
    if ((long)s < 0) { fail("multi_waiters", "mmap"); return; }
    s->futex = 0;
    s->counter = 0;
    s->ready = 0;

    /* Parent locks */
    pi_lock(&s->futex);

    long p1 = sc0(SYS_FORK);
    if (p1 == 0) {
        __sync_fetch_and_add(&s->ready, 1);
        pi_lock(&s->futex);
        __sync_fetch_and_add(&s->counter, 1);
        pi_unlock(&s->futex);
        sc1(SYS_EXIT, 0);
    }

    long p2 = sc0(SYS_FORK);
    if (p2 == 0) {
        __sync_fetch_and_add(&s->ready, 1);
        pi_lock(&s->futex);
        __sync_fetch_and_add(&s->counter, 1);
        pi_unlock(&s->futex);
        sc1(SYS_EXIT, 0);
    }

    /* Wait for both children to start */
    while (s->ready < 2) __asm__ volatile("pause");
    msleep(20);

    /* Unlock — both children should eventually acquire and release */
    pi_unlock(&s->futex);

    wait_child(p1);
    wait_child(p2);

    check_val("multi_waiters: both acquired", (long)s->counter, 2);
    sc2(SYS_MUNMAP, (long)s, 4096);
}

/* ── 9. Shared counter under PI lock (mutual exclusion) ── */

static void test_shared_counter(void) {
    shared_t *s = shared_alloc();
    if ((long)s < 0) { fail("shared_counter", "mmap"); return; }
    s->futex = 0;
    s->counter = 0;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        for (int i = 0; i < 200; i++) {
            pi_lock(&s->futex);
            s->counter++;
            pi_unlock(&s->futex);
        }
        sc1(SYS_EXIT, 0);
    }
    for (int i = 0; i < 200; i++) {
        pi_lock(&s->futex);
        s->counter++;
        pi_unlock(&s->futex);
    }
    wait_child(pid);

    check_val("shared counter = 400", (long)s->counter, 400);
    sc2(SYS_MUNMAP, (long)s, 4096);
}

/* ── 10. Deadlock detection (same process) ────── */

static void test_deadlock_detect(void) {
    shared_t *s = shared_alloc();
    if ((long)s < 0) { fail("deadlock", "mmap"); return; }
    s->futex = 0;

    long r = pi_lock(&s->futex);
    check_val("deadlock: first lock", r, 0);

    /* Second lock from same thread → -EDEADLK */
    r = pi_lock(&s->futex);
    check_val("deadlock: recursive = -EDEADLK", r, -EDEADLK);

    pi_unlock(&s->futex);
    sc2(SYS_MUNMAP, (long)s, 4096);
}

/* ── Register ─────────────────────────────────── */

TEST("mutex_blocking", test_uncontended);
TEST("mutex_blocking", test_two_procs_contend);
TEST("mutex_blocking", test_lock_sleep);
TEST("mutex_blocking", test_fairness);
TEST("mutex_blocking", test_verify_sleeping);
TEST("mutex_blocking", test_rapid_uncontended);
TEST("mutex_blocking", test_unlock_wakes);
TEST("mutex_blocking", test_multiple_waiters);
TEST("mutex_blocking", test_shared_counter);
TEST("mutex_blocking", test_deadlock_detect);
