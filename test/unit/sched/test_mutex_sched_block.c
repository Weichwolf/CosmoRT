/* RT Mutex sched_block tests — fork + MAP_SHARED, no CLONE_THREAD */
#include "ktest.h"

#define FUTEX_LOCK_PI       6
#define FUTEX_UNLOCK_PI     7
#define FUTEX_TID_MASK      0x3FFFFFFFU

#define NSEC_PER_MSEC       1000000L
#define NSEC_PER_SEC        1000000000L
#define PAGE_SIZE           4096

#define SCHED_FIFO_MSB      1

struct sched_param_msb { int sched_priority; };

typedef struct {
    volatile uint32_t futex;
    volatile int counter;
    volatile int ready;
    volatile int done;
    volatile int acquired;
    volatile int order[4];
    volatile int order_idx;
    volatile int prio_boosted;
} msb_shared_t;

static msb_shared_t *msb_alloc(void) {
    return (msb_shared_t *)(long)sc6(SYS_MMAP, 0, PAGE_SIZE,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
}

static void msb_wait_child(long pid) {
    int ws;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
}

static void msb_sleep(int ms) {
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = ms * NSEC_PER_MSEC };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
}

static long msb_pi_lock(volatile uint32_t *f) {
    return sc6(SYS_FUTEX, (long)f, FUTEX_LOCK_PI, 0, 0, 0, 0);
}

static long msb_pi_unlock(volatile uint32_t *f) {
    return sc6(SYS_FUTEX, (long)f, FUTEX_UNLOCK_PI, 0, 0, 0, 0);
}

static long msb_now_ns(void) {
    struct k_timespec ts;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&ts);
    return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

static void test_msb_uncontended(void) {
    puts("\n[mutex_sched_block]\n");
    msb_shared_t *s = msb_alloc();
    if ((long)s < 0) { fail("uncontended", "mmap"); return; }
    s->futex = 0;
    int ok = 1;
    for (int i = 0; i < 1000; i++) {
        if (msb_pi_lock(&s->futex) != 0) { ok = 0; break; }
        if (msb_pi_unlock(&s->futex) != 0) { ok = 0; break; }
    }
    check("uncontended 1000x", ok);
    sc2(SYS_MUNMAP, (long)s, PAGE_SIZE);
}

static void test_msb_two_procs(void) {
    msb_shared_t *s = msb_alloc();
    if ((long)s < 0) { fail("two_procs", "mmap"); return; }
    s->futex = 0;
    s->counter = 0;
    long p1 = sc0(SYS_FORK);
    if (p1 == 0) {
        for (int i = 0; i < 100; i++) {
            msb_pi_lock(&s->futex);
            s->counter++;
            msb_pi_unlock(&s->futex);
        }
        sc1(SYS_EXIT, 0);
    }
    long p2 = sc0(SYS_FORK);
    if (p2 == 0) {
        for (int i = 0; i < 100; i++) {
            msb_pi_lock(&s->futex);
            s->counter++;
            msb_pi_unlock(&s->futex);
        }
        sc1(SYS_EXIT, 0);
    }
    msb_wait_child(p1);
    msb_wait_child(p2);
    check_val("two_procs 200 cycles", (long)s->counter, 200);
    sc2(SYS_MUNMAP, (long)s, PAGE_SIZE);
}

static void test_msb_lock_sleep(void) {
    msb_shared_t *s = msb_alloc();
    if ((long)s < 0) { fail("lock_sleep", "mmap"); return; }
    s->futex = 0;
    s->ready = 0;
    s->acquired = 0;
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        msb_pi_lock(&s->futex);
        s->ready = 1;
        __sync_synchronize();
        msb_sleep(20);
        msb_pi_unlock(&s->futex);
        sc1(SYS_EXIT, 0);
    }
    while (!s->ready) __asm__ volatile("pause");
    long r = msb_pi_lock(&s->futex);
    s->acquired = 1;
    msb_pi_unlock(&s->futex);
    msb_wait_child(pid);
    check_val("lock_sleep: acquired after holder slept", r, 0);
    sc2(SYS_MUNMAP, (long)s, PAGE_SIZE);
}

static void test_msb_pi_boost(void) {
    msb_shared_t *s = msb_alloc();
    if ((long)s < 0) { fail("pi_boost", "mmap"); return; }
    s->futex = 0;
    s->ready = 0;
    s->prio_boosted = 0;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct sched_param_msb sp = { .sched_priority = 5 };
        sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_FIFO_MSB, (long)&sp);
        msb_pi_lock(&s->futex);
        s->ready = 1;
        __sync_synchronize();
        for (int i = 0; i < 100; i++) {
            struct sched_param_msb cp = {0};
            sc2(SYS_SCHED_GETPARAM, 0, (long)&cp);
            if (cp.sched_priority > 5) {
                s->prio_boosted = cp.sched_priority;
                break;
            }
            msb_sleep(5);
        }
        msb_pi_unlock(&s->futex);
        sc1(SYS_EXIT, 0);
    }
    while (!s->ready) __asm__ volatile("pause");
    struct sched_param_msb sp = { .sched_priority = 20 };
    sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_FIFO_MSB, (long)&sp);
    msb_pi_lock(&s->futex);
    msb_pi_unlock(&s->futex);
    msb_wait_child(pid);
    check("pi_boost: owner priority raised", s->prio_boosted >= 20);
    struct sched_param_msb sp0 = { .sched_priority = 0 };
    sc3(SYS_SCHED_SETSCHEDULER, 0, 0, (long)&sp0);
    sc2(SYS_MUNMAP, (long)s, PAGE_SIZE);
}

static void test_msb_four_procs(void) {
    msb_shared_t *s = msb_alloc();
    if ((long)s < 0) { fail("four_procs", "mmap"); return; }
    s->futex = 0;
    s->counter = 0;
    long pids[4];
    for (int i = 0; i < 4; i++) {
        pids[i] = sc0(SYS_FORK);
        if (pids[i] == 0) {
            for (int j = 0; j < 50; j++) {
                msb_pi_lock(&s->futex);
                s->counter++;
                msb_pi_unlock(&s->futex);
            }
            sc1(SYS_EXIT, 0);
        }
    }
    for (int i = 0; i < 4; i++)
        msb_wait_child(pids[i]);
    check_val("four_procs 200 cycles", (long)s->counter, 200);
    sc2(SYS_MUNMAP, (long)s, PAGE_SIZE);
}

static void test_msb_rapid_contention(void) {
    msb_shared_t *s = msb_alloc();
    if ((long)s < 0) { fail("rapid_contention", "mmap"); return; }
    s->futex = 0;
    s->counter = 0;
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        for (int i = 0; i < 200; i++) {
            msb_pi_lock(&s->futex);
            s->counter++;
            msb_pi_unlock(&s->futex);
        }
        sc1(SYS_EXIT, 0);
    }
    for (int i = 0; i < 200; i++) {
        msb_pi_lock(&s->futex);
        s->counter++;
        msb_pi_unlock(&s->futex);
    }
    msb_wait_child(pid);
    check_val("rapid_contention 400 cycles", (long)s->counter, 400);
    sc2(SYS_MUNMAP, (long)s, PAGE_SIZE);
}

static void test_msb_lock_ordering(void) {
    msb_shared_t *s = msb_alloc();
    if ((long)s < 0) { fail("lock_ordering", "mmap"); return; }
    s->futex = 0;
    s->order_idx = 0;
    s->order[0] = 0;
    s->order[1] = 0;
    s->ready = 0;
    msb_pi_lock(&s->futex);
    long p1 = sc0(SYS_FORK);
    if (p1 == 0) {
        struct sched_param_msb sp = { .sched_priority = 20 };
        sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_FIFO_MSB, (long)&sp);
        __sync_fetch_and_add(&s->ready, 1);
        msb_pi_lock(&s->futex);
        int idx = __sync_fetch_and_add(&s->order_idx, 1);
        s->order[idx] = 20;
        msb_pi_unlock(&s->futex);
        sc1(SYS_EXIT, 0);
    }
    long p2 = sc0(SYS_FORK);
    if (p2 == 0) {
        struct sched_param_msb sp = { .sched_priority = 10 };
        sc3(SYS_SCHED_SETSCHEDULER, 0, SCHED_FIFO_MSB, (long)&sp);
        __sync_fetch_and_add(&s->ready, 1);
        msb_pi_lock(&s->futex);
        int idx = __sync_fetch_and_add(&s->order_idx, 1);
        s->order[idx] = 10;
        msb_pi_unlock(&s->futex);
        sc1(SYS_EXIT, 0);
    }
    while (s->ready < 2) __asm__ volatile("pause");
    msb_sleep(20);
    msb_pi_unlock(&s->futex);
    msb_wait_child(p1);
    msb_wait_child(p2);
    check_val("lock_ordering: both completed", (long)s->order_idx, 2);
    int has10 = 0, has20 = 0;
    for (int i = 0; i < 2; i++) {
        if (s->order[i] == 10) has10 = 1;
        if (s->order[i] == 20) has20 = 1;
    }
    check("lock_ordering: both prios present", has10 && has20);
    struct sched_param_msb sp0 = { .sched_priority = 0 };
    sc3(SYS_SCHED_SETSCHEDULER, 0, 0, (long)&sp0);
    sc2(SYS_MUNMAP, (long)s, PAGE_SIZE);
}

static void test_msb_unlock_wakes(void) {
    msb_shared_t *s = msb_alloc();
    if ((long)s < 0) { fail("unlock_wakes", "mmap"); return; }
    s->futex = 0;
    s->ready = 0;
    s->done = 0;
    msb_pi_lock(&s->futex);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        s->ready = 1;
        __sync_synchronize();
        msb_pi_lock(&s->futex);
        s->done = 1;
        msb_pi_unlock(&s->futex);
        sc1(SYS_EXIT, 0);
    }
    while (!s->ready) __asm__ volatile("pause");
    msb_sleep(10);
    check("unlock_wakes: child blocked", s->done == 0);
    msb_pi_unlock(&s->futex);
    msb_wait_child(pid);
    check_val("unlock_wakes: child resumed", (long)s->done, 1);
    sc2(SYS_MUNMAP, (long)s, PAGE_SIZE);
}

static void test_msb_trylock(void) {
    msb_shared_t *s = msb_alloc();
    if ((long)s < 0) { fail("trylock", "mmap"); return; }
    s->futex = 0;
    long tid = sc0(SYS_GETTID);
    uint32_t old = __sync_val_compare_and_swap(&s->futex, 0, (uint32_t)tid);
    check_val("trylock: CAS succeed", (long)old, 0);
    old = __sync_val_compare_and_swap(&s->futex, 0, (uint32_t)tid);
    check("trylock: CAS fail (locked)", old != 0);
    msb_pi_unlock(&s->futex);
    sc2(SYS_MUNMAP, (long)s, PAGE_SIZE);
}

static void test_msb_deadlock(void) {
    msb_shared_t *s = msb_alloc();
    if ((long)s < 0) { fail("deadlock", "mmap"); return; }
    s->futex = 0;
    long r = msb_pi_lock(&s->futex);
    check_val("deadlock: first lock", r, 0);
    r = msb_pi_lock(&s->futex);
    check_val("deadlock: recursive = -EDEADLK", r, -EDEADLK);
    msb_pi_unlock(&s->futex);
    sc2(SYS_MUNMAP, (long)s, PAGE_SIZE);
}

static void test_msb_pipe_under_contention(void) {
    msb_shared_t *s = msb_alloc();
    if ((long)s < 0) { fail("pipe_mutex", "mmap"); return; }
    s->futex = 0;
    s->counter = 0;
    int pfd[2];
    sc1(SYS_PIPE2, (long)pfd);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        for (int i = 0; i < 10; i++) {
            msb_pi_lock(&s->futex);
            s->counter++;
            msb_pi_unlock(&s->futex);
        }
        char c = 'D';
        sc3(SYS_WRITE, pfd[1], (long)&c, 1);
        sc1(SYS_EXIT, 0);
    }
    for (int i = 0; i < 10; i++) {
        msb_pi_lock(&s->futex);
        s->counter++;
        msb_pi_unlock(&s->futex);
    }
    char r = 0;
    sc3(SYS_READ, pfd[0], (long)&r, 1);
    msb_wait_child(pid);
    check("pipe_under_contention: pipe ok", r == 'D');
    check_val("pipe_under_contention: counter=20", (long)s->counter, 20);
    sc1(SYS_CLOSE, pfd[0]);
    sc1(SYS_CLOSE, pfd[1]);
    sc2(SYS_MUNMAP, (long)s, PAGE_SIZE);
}

static void test_msb_nanosleep_under_contention(void) {
    msb_shared_t *s = msb_alloc();
    if ((long)s < 0) { fail("nanosleep_mutex", "mmap"); return; }
    s->futex = 0;
    s->counter = 0;
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        for (int i = 0; i < 5; i++) {
            msb_pi_lock(&s->futex);
            msb_sleep(2);
            s->counter++;
            msb_pi_unlock(&s->futex);
        }
        sc1(SYS_EXIT, 0);
    }
    for (int i = 0; i < 5; i++) {
        msb_pi_lock(&s->futex);
        msb_sleep(2);
        s->counter++;
        msb_pi_unlock(&s->futex);
    }
    msb_wait_child(pid);
    long t0 = msb_now_ns();
    (void)t0;
    check_val("nanosleep_under_contention: 10 cycles", (long)s->counter, 10);
    sc2(SYS_MUNMAP, (long)s, PAGE_SIZE);
}

TEST("msb_uncontended",      test_msb_uncontended);
TEST("msb_two_procs",        test_msb_two_procs);
TEST("msb_lock_sleep",       test_msb_lock_sleep);
TEST("msb_pi_boost",         test_msb_pi_boost);
TEST("msb_four_procs",       test_msb_four_procs);
TEST("msb_rapid_contention", test_msb_rapid_contention);
TEST("msb_lock_ordering",    test_msb_lock_ordering);
TEST("msb_unlock_wakes",     test_msb_unlock_wakes);
TEST("msb_trylock",          test_msb_trylock);
TEST("msb_deadlock",         test_msb_deadlock);
TEST("msb_pipe_mutex",       test_msb_pipe_under_contention);
TEST("msb_nanosleep_mutex",  test_msb_nanosleep_under_contention);
