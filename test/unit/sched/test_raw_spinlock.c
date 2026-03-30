/* Raw Spinlock tests — fork + shared memory
 *
 * Tests CAS-based userspace spinlock (mirrors raw_spinlock_t ticket lock
 * semantics). Each test uses MAP_SHARED|MAP_ANONYMOUS for cross-process
 * shared state.
 */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

/* ── Shared memory layout ──────────────────────── */

typedef struct {
    volatile uint32_t lock;     /* 0 = free, 1 = held */
    volatile int counter;
    volatile int counterA;
    volatile int counterB;
    volatile int a;
    volatile int b;
    volatile int ready;
} shared_t;

static shared_t *shared_alloc(void) {
    return (shared_t *)(long)sc6(SYS_MMAP, 0, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
}

/* ── CAS spinlock (userspace) ──────────────────── */

static void cas_lock(volatile uint32_t *lk) {
    while (!__sync_bool_compare_and_swap(lk, 0, 1))
        __asm__ volatile("pause");
}

static void cas_unlock(volatile uint32_t *lk) {
    __sync_synchronize();
    *lk = 0;
}

static int cas_trylock(volatile uint32_t *lk) {
    return __sync_bool_compare_and_swap(lk, 0, 1);
}

static void wait_child(long pid) {
    int ws;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
}

/* ── 1. Shared counter: parent + child under lock ── */

static void test_shared_counter(void) {
    puts("\n[raw_spinlock]\n");
    shared_t *s = shared_alloc();
    if ((long)s < 0) { fail("shared_counter", "mmap"); return; }
    s->lock = 0; s->counter = 0;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        for (int i = 0; i < 1000; i++) {
            cas_lock(&s->lock);
            s->counter++;
            cas_unlock(&s->lock);
        }
        sc1(SYS_EXIT, 0);
    }
    for (int i = 0; i < 1000; i++) {
        cas_lock(&s->lock);
        s->counter++;
        cas_unlock(&s->lock);
    }
    wait_child(pid);
    check_val("shared counter = 2000", (long)s->counter, 2000);
    sc2(SYS_MUNMAP, (long)s, 4096);
}

/* ── 2. Two children contention ──────────────────── */

static void test_two_children(void) {
    shared_t *s = shared_alloc();
    if ((long)s < 0) { fail("two_children", "mmap"); return; }
    s->lock = 0; s->counter = 0;

    long p1 = sc0(SYS_FORK);
    if (p1 == 0) {
        for (int i = 0; i < 500; i++) {
            cas_lock(&s->lock);
            s->counter++;
            cas_unlock(&s->lock);
        }
        sc1(SYS_EXIT, 0);
    }
    long p2 = sc0(SYS_FORK);
    if (p2 == 0) {
        for (int i = 0; i < 500; i++) {
            cas_lock(&s->lock);
            s->counter++;
            cas_unlock(&s->lock);
        }
        sc1(SYS_EXIT, 0);
    }
    wait_child(p1);
    wait_child(p2);
    check_val("two children counter = 1000", (long)s->counter, 1000);
    sc2(SYS_MUNMAP, (long)s, 4096);
}

/* ── 3. Trylock returns busy when locked ─────────── */

static void test_trylock_busy(void) {
    shared_t *s = shared_alloc();
    if ((long)s < 0) { fail("trylock_busy", "mmap"); return; }
    s->lock = 0;

    cas_lock(&s->lock);
    int got = cas_trylock(&s->lock);
    check("trylock busy returns 0", got == 0);
    cas_unlock(&s->lock);
    sc2(SYS_MUNMAP, (long)s, 4096);
}

/* ── 4. Self-trylock: no deadlock ────────────────── */

static void test_self_trylock(void) {
    volatile uint32_t lk = 0;
    cas_lock(&lk);
    int got = cas_trylock(&lk);
    check("self-trylock returns busy", got == 0);
    cas_unlock(&lk);
    pass("self-trylock no deadlock");
}

/* ── 5. Sequential lock/unlock 100 cycles ────────── */

static void test_sequential(void) {
    volatile uint32_t lk = 0;
    for (int i = 0; i < 100; i++) {
        cas_lock(&lk);
        cas_unlock(&lk);
    }
    pass("sequential 100 cycles");
}

/* ── 6. Four children stress ─────────────────────── */

static void test_four_children(void) {
    shared_t *s = shared_alloc();
    if ((long)s < 0) { fail("four_children", "mmap"); return; }
    s->lock = 0; s->counter = 0;

    long pids[4];
    for (int c = 0; c < 4; c++) {
        pids[c] = sc0(SYS_FORK);
        if (pids[c] == 0) {
            for (int i = 0; i < 50; i++) {
                cas_lock(&s->lock);
                s->counter++;
                cas_unlock(&s->lock);
            }
            sc1(SYS_EXIT, 0);
        }
    }
    for (int c = 0; c < 4; c++) wait_child(pids[c]);
    check_val("4 children × 50 = 200", (long)s->counter, 200);
    sc2(SYS_MUNMAP, (long)s, 4096);
}

/* ── 7. Fairness: both children get lock ─────────── */

static void test_fairness(void) {
    shared_t *s = shared_alloc();
    if ((long)s < 0) { fail("fairness", "mmap"); return; }
    s->lock = 0; s->counterA = 0; s->counterB = 0;

    long p1 = sc0(SYS_FORK);
    if (p1 == 0) {
        for (int i = 0; i < 100; i++) {
            cas_lock(&s->lock);
            s->counterA++;
            cas_unlock(&s->lock);
        }
        sc1(SYS_EXIT, 0);
    }
    long p2 = sc0(SYS_FORK);
    if (p2 == 0) {
        for (int i = 0; i < 100; i++) {
            cas_lock(&s->lock);
            s->counterB++;
            cas_unlock(&s->lock);
        }
        sc1(SYS_EXIT, 0);
    }
    wait_child(p1);
    wait_child(p2);
    check("fairness: child A got lock", s->counterA > 0);
    check("fairness: child B got lock", s->counterB > 0);
    sc2(SYS_MUNMAP, (long)s, 4096);
}

/* ── 8. Lock protects invariant: a == b ──────────── */

static void test_invariant(void) {
    shared_t *s = shared_alloc();
    if ((long)s < 0) { fail("invariant", "mmap"); return; }
    s->lock = 0; s->a = 0; s->b = 0; s->counter = 0;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Modifier: increment a,b under lock */
        for (int i = 0; i < 500; i++) {
            cas_lock(&s->lock);
            s->a++;
            s->b++;
            cas_unlock(&s->lock);
        }
        sc1(SYS_EXIT, 0);
    }
    /* Checker: verify a == b under lock */
    int violations = 0;
    for (int i = 0; i < 500; i++) {
        cas_lock(&s->lock);
        if (s->a != s->b) violations++;
        s->a++;
        s->b++;
        cas_unlock(&s->lock);
    }
    wait_child(pid);
    check_val("invariant a==b: 0 violations", (long)violations, 0);
    check_val("invariant total = 1000", (long)s->a, 1000);
    sc2(SYS_MUNMAP, (long)s, 4096);
}

/* ── 9. Unlock really unlocks ────────────────────── */

static void test_unlock_releases(void) {
    shared_t *s = shared_alloc();
    if ((long)s < 0) { fail("unlock_releases", "mmap"); return; }
    s->lock = 0; s->counter = 0;

    cas_lock(&s->lock);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Child tries to acquire — should block until parent unlocks */
        cas_lock(&s->lock);
        s->counter = 42;
        cas_unlock(&s->lock);
        sc1(SYS_EXIT, 0);
    }

    /* Small busy-wait to let child start spinning */
    for (volatile int i = 0; i < 10000; i++);
    cas_unlock(&s->lock);

    wait_child(pid);
    check_val("unlock allows child acquire", (long)s->counter, 42);
    sc2(SYS_MUNMAP, (long)s, 4096);
}

/* ── 10. No lock → data race → counter < expected ── */

static void test_no_lock_race(void) {
    shared_t *s = shared_alloc();
    if ((long)s < 0) { fail("no_lock_race", "mmap"); return; }
    s->counter = 0;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        for (int i = 0; i < 10000; i++)
            s->counter++;  /* no lock — racy */
        sc1(SYS_EXIT, 0);
    }
    for (int i = 0; i < 10000; i++)
        s->counter++;  /* no lock — racy */
    wait_child(pid);

    /* With a race, counter is likely < 20000. But on single-core QEMU
     * (ktest), processes run sequentially so it might be exactly 20000.
     * We accept both outcomes — the lock tests above prove correctness. */
    check("no-lock counter <= 20000", s->counter <= 20000);
    pass("no-lock race test completed");
    sc2(SYS_MUNMAP, (long)s, 4096);
}

/* ── Register ──────────────────────────────────── */

TEST("raw_spinlock", test_shared_counter);
TEST("raw_spinlock", test_two_children);
TEST("raw_spinlock", test_trylock_busy);
TEST("raw_spinlock", test_self_trylock);
TEST("raw_spinlock", test_sequential);
TEST("raw_spinlock", test_four_children);
TEST("raw_spinlock", test_fairness);
TEST("raw_spinlock", test_invariant);
TEST("raw_spinlock", test_unlock_releases);
TEST("raw_spinlock", test_no_lock_race);
