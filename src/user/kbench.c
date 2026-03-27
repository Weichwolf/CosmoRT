/* CosmoRT Kernel Benchmark — multi-core stress + latency measurement */

typedef unsigned long uint64_t;
typedef long int64_t;
typedef unsigned int uint32_t;
typedef unsigned long size_t;

#define NULL ((void *)0)

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static long syscall0(long n) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n) : "rcx","r11","memory");
    return ret;
}

static long syscall1(long n, long a1) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1) : "rcx","r11","memory");
    return ret;
}

static long syscall2(long n, long a1, long a2) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2) : "rcx","r11","memory");
    return ret;
}

static long syscall3(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx","r11","memory");
    return ret;
}

static long syscall5(long n, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx","r11","memory");
    return ret;
}

static long syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx","r11","memory");
    return ret;
}

#define SYS_write          1
#define SYS_mmap           9
#define SYS_munmap         11
#define SYS_getpid         39
#define SYS_clone          56
#define SYS_exit           60
#define SYS_exit_group     231
#define SYS_clock_gettime  228
#define SYS_sched_yield    24
#define SYS_gettid         186

#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define MAP_PRIVATE 0x02
#define MAP_ANON    0x20

#define CLONE_VM      0x00000100
#define CLONE_FS      0x00000200
#define CLONE_FILES   0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_THREAD  0x00010000

#define PAGE_SIZE 4096
#define THREAD_STACK_SIZE (64 * 1024)

#define BENCH_ITERS 10000
#define MMAP_ITERS  1000
#define FAULT_ITERS 1000
#define NUM_WORKERS 3  /* threads share 1 core via preemption under TCG */
#define STRESS_ITERS 500000

/* ── Output ───────────────────────────────────── */

static void puts(const char *s) {
    int len = 0;
    while (s[len]) len++;
    syscall3(SYS_write, 1, (long)s, len);
}

static void put_uint(uint64_t v) {
    char buf[20]; int i = 0;
    if (v == 0) { puts("0"); return; }
    while (v) { buf[i++] = '0' + (char)(v % 10); v /= 10; }
    char out[20]; int j = 0;
    while (i--) out[j++] = buf[i];
    out[j] = 0;
    puts(out);
}

static void put_result(const char *name, uint64_t cycles, uint64_t iters) {
    puts("  ");
    puts(name);
    puts(": ");
    put_uint(cycles / iters);
    puts(" cyc/op (");
    put_uint(iters);
    puts(" iters)\n");
}

/* ── Shared state for multi-core stress ──────── */

static volatile uint64_t worker_counter[8];  /* per-core iteration count */
static volatile int       workers_go = 0;     /* start signal */
static volatile int       workers_stop = 0;   /* stop signal */

/* Worker: burn CPU, increment counter */
static void worker_fn(void) {
    int tid = (int)syscall0(SYS_gettid);
    int slot = tid & 7;

    /* Wait for start signal */
    while (!workers_go)
        __asm__ volatile("pause");

    /* Burn CPU for fixed iterations (pure compute, no syscalls) */
    uint64_t count = 0;
    volatile uint64_t x = 1;
    while (count < STRESS_ITERS) {
        x = x * 6364136223846793005ULL + 1;
        count++;
    }

    worker_counter[slot] = count;
    __sync_fetch_and_add(&workers_stop, 1);  /* signal completion */
    for (;;) __asm__ volatile("pause");      /* don't exit — stay alive */
}

/* ── Benchmarks ──────────────────────────────── */

static void bench_syscall(void) {
    for (int i = 0; i < 100; i++) syscall0(SYS_getpid);
    uint64_t start = rdtsc();
    for (int i = 0; i < BENCH_ITERS; i++)
        syscall0(SYS_getpid);
    uint64_t end = rdtsc();
    put_result("getpid", end - start, BENCH_ITERS);
}

static void bench_clock(void) {
    struct { long tv_sec; long tv_nsec; } ts;
    uint64_t start = rdtsc();
    for (int i = 0; i < BENCH_ITERS; i++)
        syscall2(SYS_clock_gettime, 1, (long)&ts);
    uint64_t end = rdtsc();
    put_result("clock_gettime", end - start, BENCH_ITERS);
}

static void bench_yield(void) {
    uint64_t start = rdtsc();
    for (int i = 0; i < BENCH_ITERS; i++)
        syscall0(SYS_sched_yield);
    uint64_t end = rdtsc();
    put_result("sched_yield", end - start, BENCH_ITERS);
}

static void bench_pagefault(void) {
    size_t region = (size_t)FAULT_ITERS * PAGE_SIZE;
    long addr = syscall6(SYS_mmap, 0, (long)region,
                         PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (addr < 0) { puts("  pagefault: mmap failed\n"); return; }
    uint64_t start = rdtsc();
    volatile char *p = (volatile char *)addr;
    for (int i = 0; i < FAULT_ITERS; i++)
        p[(size_t)i * PAGE_SIZE] = (char)i;
    uint64_t end = rdtsc();
    put_result("page fault", end - start, FAULT_ITERS);
    syscall2(SYS_munmap, addr, (long)region);
}

static void bench_mmap(void) {
    uint64_t start = rdtsc();
    for (int i = 0; i < MMAP_ITERS; i++) {
        long addr = syscall6(SYS_mmap, 0, PAGE_SIZE,
                             PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (addr > 0) syscall2(SYS_munmap, addr, PAGE_SIZE);
    }
    uint64_t end = rdtsc();
    put_result("mmap+munmap", end - start, MMAP_ITERS);
}

/* ── Multi-Core Stress ───────────────────────── */

static void bench_multicore(void) {
    puts("\n[Multi-Core Stress: ");
    put_uint(NUM_WORKERS + 1);
    puts(" threads, ");
    put_uint(STRESS_ITERS);
    puts(" iters each]\n");

    /* Spawn worker threads via clone() */
    for (int i = 0; i < NUM_WORKERS; i++) {
        long stack = syscall6(SYS_mmap, 0, THREAD_STACK_SIZE,
                              PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (stack < 0) { puts("  mmap stack failed\n"); return; }

        long ret = syscall5(SYS_clone,
                            CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD,
                            stack + THREAD_STACK_SIZE, 0, 0, 0);
        if (ret == 0) {
            /* Child: immediately jump to worker (no parent stack access) */
            worker_fn();
            __builtin_unreachable();
        }
        if (ret < 0) {
            puts("  clone failed\n");
            return;
        }
        puts("  tid=");
        put_uint((uint64_t)ret);
    }
    puts("\n");

    /* Start workers */
    __asm__ volatile("mfence" ::: "memory");
    workers_go = 1;
    __asm__ volatile("mfence" ::: "memory");

    /* Main thread burns CPU for fixed iterations */
    uint64_t main_count = 0;
    uint64_t start = rdtsc();
    volatile uint64_t x = 1;
    while (main_count < STRESS_ITERS) {
        x = x * 6364136223846793005ULL + 1;
        main_count++;
    }
    uint64_t elapsed = rdtsc() - start;

    /* Stop workers */
    __asm__ volatile("mfence" ::: "memory");
    workers_stop = 1;
    __asm__ volatile("mfence" ::: "memory");

    /* Wait for all workers to complete */
    while (workers_stop < NUM_WORKERS)
        __asm__ volatile("pause");

    /* Report */
    puts("  main:   ");
    put_uint(main_count);
    puts(" iters (");
    put_uint(elapsed);
    puts(" cyc)\n");
    uint64_t total = main_count;
    for (int i = 0; i < 8; i++) {
        if (worker_counter[i]) {
            puts("  worker: ");
            put_uint(worker_counter[i]);
            puts(" iters\n");
            total += worker_counter[i];
        }
    }
    puts("  total:  ");
    put_uint(total);
    puts(" iters (");
    put_uint(NUM_WORKERS + 1);
    puts(" threads)\n");
}

/* ── Main ────────────────────────────────────── */

void _start_c(void) {
    puts("\n=== CosmoRT Kernel Benchmark ===\n\n");

    puts("[Syscall Latency]\n");
    bench_syscall();
    bench_clock();
    bench_yield();

    puts("\n[Memory]\n");
    bench_pagefault();
    bench_mmap();

    bench_multicore();

    puts("\n=== Benchmark Complete ===\n");
    syscall1(SYS_exit_group, 0);
    __builtin_unreachable();
}
