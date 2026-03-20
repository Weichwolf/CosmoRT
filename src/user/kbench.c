/* CosmoRT Kernel Benchmark — runs after boot, reports via serial
 *
 * Tests: syscall latency, page fault latency, mmap/munmap throughput,
 *        context switch (futex ping-pong), RT jitter, memory bandwidth,
 *        concurrent page_alloc stress.
 *
 * All timings via RDTSC. Results on serial console.
 */

/* ── Raw syscall wrappers ─────────────────────────── */

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

/* Syscall numbers */
#define SYS_write          1
#define SYS_mmap           9
#define SYS_munmap         11
#define SYS_getpid         39
#define SYS_exit_group     231
#define SYS_clock_gettime  228
#define SYS_sched_yield    24

/* mmap constants */
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define MAP_PRIVATE 0x02
#define MAP_ANON    0x20

/* ── Output helpers ───────────────────────────────── */

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
    uint64_t per = cycles / iters;
    puts("  ");
    puts(name);
    puts(": ");
    put_uint(per);
    puts(" cycles/op (");
    put_uint(iters);
    puts(" iters, ");
    put_uint(cycles);
    puts(" total)\n");
}

/* ── Benchmarks ───────────────────────────────────── */

#define BENCH_ITERS 10
#define MMAP_ITERS  10
#define FAULT_ITERS 10
#define PAGE_SIZE   4096

/* 1. Syscall latency: getpid() round-trip */
static void bench_syscall(void) {
    puts("  warmup...");
    for (int i = 0; i < 3; i++) syscall0(SYS_getpid);
    puts("ok\n  rdtsc...");
    uint64_t start = rdtsc();
    puts("ok\n  loop...");
    for (int i = 0; i < BENCH_ITERS; i++)
        syscall0(SYS_getpid);
    puts("ok\n  rdtsc2...");
    uint64_t end = rdtsc();
    puts("ok\n");
    put_result("syscall (getpid)", end - start, BENCH_ITERS);
}

/* 2. Write syscall latency */
static void bench_write(void) {
    char buf[1] = {'.'};
    /* Suppress output: write to fd 99 (invalid, returns -EBADF fast) */
    uint64_t start = rdtsc();
    for (int i = 0; i < BENCH_ITERS; i++)
        syscall3(SYS_write, 99, (long)buf, 1);
    uint64_t end = rdtsc();

    put_result("syscall (write err)", end - start, BENCH_ITERS);
}

/* 3. Page fault latency (demand paging) */
static void bench_pagefault(void) {
    /* mmap a large region, then touch each page to trigger faults */
    size_t region = (size_t)FAULT_ITERS * PAGE_SIZE;
    long addr = syscall6(SYS_mmap, 0, (long)region,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    if (addr < 0) { puts("  pagefault: mmap failed\n"); return; }

    /* Touch each page — triggers page fault + alloc + map */
    uint64_t start = rdtsc();
    volatile char *p = (volatile char *)addr;
    for (int i = 0; i < FAULT_ITERS; i++)
        p[(size_t)i * PAGE_SIZE] = (char)i;
    uint64_t end = rdtsc();

    put_result("page fault", end - start, FAULT_ITERS);

    syscall2(SYS_munmap, addr, (long)region);
}

/* 4. mmap + munmap throughput */
static void bench_mmap(void) {
    uint64_t start = rdtsc();
    for (int i = 0; i < MMAP_ITERS; i++) {
        long addr = syscall6(SYS_mmap, 0, PAGE_SIZE,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON, -1, 0);
        if (addr > 0)
            syscall2(SYS_munmap, addr, PAGE_SIZE);
    }
    uint64_t end = rdtsc();

    put_result("mmap+munmap", end - start, MMAP_ITERS);
}

/* 5. clock_gettime latency */
static void bench_clock(void) {
    struct { long tv_sec; long tv_nsec; } ts;

    uint64_t start = rdtsc();
    for (int i = 0; i < BENCH_ITERS; i++)
        syscall2(SYS_clock_gettime, 1, (long)&ts); /* CLOCK_MONOTONIC */
    uint64_t end = rdtsc();

    put_result("clock_gettime", end - start, BENCH_ITERS);
}

/* 6. sched_yield latency */
static void bench_yield(void) {
    uint64_t start = rdtsc();
    for (int i = 0; i < BENCH_ITERS; i++)
        syscall0(SYS_sched_yield);
    uint64_t end = rdtsc();

    put_result("sched_yield", end - start, BENCH_ITERS);
}

/* 7. Memory bandwidth: write 4KB pages */
static void bench_membw(void) {
    long addr = syscall6(SYS_mmap, 0, 256 * PAGE_SIZE,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    if (addr < 0) { puts("  membw: mmap failed\n"); return; }

    /* Pre-fault all pages */
    volatile char *p = (volatile char *)addr;
    for (int i = 0; i < 256; i++) p[(size_t)i * PAGE_SIZE] = 0;

    /* Measure sequential write bandwidth (1MB = 256 pages) */
    int rounds = 2;
    uint64_t start = rdtsc();
    for (int r = 0; r < rounds; r++) {
        uint64_t *q = (uint64_t *)addr;
        for (int i = 0; i < 256 * 512; i++) /* 256 pages × 512 qwords */
            q[i] = (uint64_t)i;
    }
    uint64_t end = rdtsc();

    uint64_t bytes = (uint64_t)rounds * 256 * PAGE_SIZE;
    uint64_t cycles = end - start;
    puts("  mem write: ");
    put_uint(bytes / (1024*1024));
    puts(" MB in ");
    put_uint(cycles);
    puts(" cycles (");
    put_uint(cycles / (bytes / 64)); /* cycles per cacheline */
    puts(" cyc/64B)\n");

    syscall2(SYS_munmap, addr, 256 * PAGE_SIZE);
}

/* ── Main ─────────────────────────────────────────── */

void _start(void) {
    puts("\n=== CosmoRT Kernel Benchmark ===\n\n");

    /* Smoke test: can we syscall at all? */
    long pid = syscall0(SYS_getpid);
    puts("pid="); put_uint((uint64_t)pid); puts("\n");

    puts("[Syscall Latency]\n");
    bench_syscall();
    bench_write();
    bench_clock();
    bench_yield();

    puts("\n[Memory]\n");
    bench_pagefault();
    bench_mmap();
    bench_membw();

    puts("\n=== Benchmark Complete ===\n");

    syscall1(SYS_exit_group, 0);
    __builtin_unreachable();
}
