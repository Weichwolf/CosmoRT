/* Kernel micro-benchmarks — syscall latency, memory, multi-core */
#include "ktest.h"

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#define BENCH_ITERS   10000
#define FAULT_ITERS   256
#define MMAP_ITERS    1000

static void bench_result(const char *name, uint64_t cycles, uint64_t iters) {
    puts("  "); puts(name); puts(": ");
    put_int((long)(cycles / iters)); puts(" cyc/op\n");
}

/* ── Syscall latency ───────────────────────────── */

static void test_bench_syscall(void) {
    puts("\n[bench syscall]\n");

    /* Warmup */
    for (int i = 0; i < 100; i++) sc0(SYS_GETPID);

    uint64_t start = rdtsc();
    for (int i = 0; i < BENCH_ITERS; i++)
        sc0(SYS_GETPID);
    uint64_t end = rdtsc();
    bench_result("getpid", end - start, BENCH_ITERS);
    check("getpid < 10000 cyc", (end - start) / BENCH_ITERS < 10000);

    struct { long sec; long nsec; } ts;
    start = rdtsc();
    for (int i = 0; i < BENCH_ITERS; i++)
        sc2(SYS_CLOCK_GETTIME, 1, (long)&ts);
    end = rdtsc();
    bench_result("clock_gettime", end - start, BENCH_ITERS);
    check("clock < 10000 cyc", (end - start) / BENCH_ITERS < 10000);

    start = rdtsc();
    for (int i = 0; i < BENCH_ITERS; i++)
        sc0(SYS_SCHED_YIELD);
    end = rdtsc();
    bench_result("sched_yield", end - start, BENCH_ITERS);
}
TEST("bench_syscall", test_bench_syscall);

/* ── Memory: page fault + mmap ─────────────────── */

static void test_bench_memory(void) {
    puts("\n[bench memory]\n");

    /* Page fault latency */
    long region = (long)FAULT_ITERS * 4096;
    long addr = sc6(SYS_MMAP, 0, region, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap for fault bench", addr > 0x1000);
    if (addr > 0x1000) {
        uint64_t start = rdtsc();
        volatile char *p = (volatile char *)addr;
        for (int i = 0; i < FAULT_ITERS; i++)
            p[(long)i * 4096] = (char)i;
        uint64_t end = rdtsc();
        bench_result("page fault", end - start, FAULT_ITERS);
        sc2(SYS_MUNMAP, addr, region);
    }

    /* mmap+munmap cycle */
    uint64_t start = rdtsc();
    for (int i = 0; i < MMAP_ITERS; i++) {
        long a = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
        if (a > 0) sc2(SYS_MUNMAP, a, 4096);
    }
    uint64_t end = rdtsc();
    bench_result("mmap+munmap", end - start, MMAP_ITERS);
}
TEST("bench_memory", test_bench_memory);

/* ── Multi-core stress ─────────────────────────── */

#define STRESS_ITERS  100000
#define NUM_WORKERS   3
#define THREAD_STACK  (16 * 4096)

static volatile uint64_t worker_counter[8];
static volatile int workers_go;
static volatile int workers_done;

static void worker_fn(void) {
    int tid = (int)sc0(SYS_GETTID);
    int slot = tid & 7;
    while (!workers_go) __asm__ volatile("pause");
    volatile uint64_t x = 1;
    uint64_t count = 0;
    while (count < STRESS_ITERS) {
        x = x * 6364136223846793005ULL + 1;
        count++;
    }
    worker_counter[slot] = count;
    __sync_fetch_and_add(&workers_done, 1);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static void test_bench_multicore(void) {
    puts("\n[bench multicore]\n");

    workers_go = 0;
    workers_done = 0;
    for (int i = 0; i < 8; i++) worker_counter[i] = 0;

    /* Spawn workers via clone */
    int spawned = 0;
    for (int i = 0; i < NUM_WORKERS; i++) {
        long stk = sc6(SYS_MMAP, 0, THREAD_STACK, PROT_RW, MAP_PRIV_ANON, -1, 0);
        if (stk < 0) break;
        long ret = sc5(SYS_CLONE,
            CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD,
            stk + THREAD_STACK, 0, 0, 0);
        if (ret == 0) { worker_fn(); __builtin_unreachable(); }
        if (ret > 0) spawned++;
    }
    check("workers spawned", spawned > 0);

    /* Start + main thread work */
    __sync_synchronize();
    workers_go = 1;
    __sync_synchronize();

    volatile uint64_t x = 1;
    uint64_t main_count = 0;
    uint64_t start = rdtsc();
    while (main_count < STRESS_ITERS) {
        x = x * 6364136223846793005ULL + 1;
        main_count++;
    }
    uint64_t elapsed = rdtsc() - start;

    /* Wait for workers */
    int tries = 0;
    while (workers_done < spawned && tries++ < 1000000)
        __asm__ volatile("pause");
    check("all workers finished", workers_done >= spawned);

    /* Report */
    uint64_t total = main_count;
    for (int i = 0; i < 8; i++) total += worker_counter[i];
    puts("  main: "); put_int((long)main_count); puts(" iters (");
    put_int((long)elapsed); puts(" cyc)\n");
    puts("  total: "); put_int((long)total); puts(" iters (");
    put_int((long)(spawned + 1)); puts(" threads)\n");
    check("total >= main", (long)total >= (long)main_count);
}
TEST("bench_multicore", test_bench_multicore);
