/* CosmoRT ktest — shared definitions for hardware tests */
#ifndef KTEST_H
#define KTEST_H

typedef unsigned long uint64_t;
typedef long int64_t;
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned char uint8_t;
typedef unsigned long size_t;
typedef long ssize_t;

#define NULL ((void *)0)

/* ── Syscall wrappers ─────────────────────────── */

static inline long sc0(long n) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n):"rcx","r11","memory"); return r;
}
static inline long sc1(long n, long a) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a):"rcx","r11","memory"); return r;
}
static inline long sc2(long n, long a, long b) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b):"rcx","r11","memory"); return r;
}
static inline long sc3(long n, long a, long b, long c) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r;
}
static inline long sc4(long n, long a, long b, long c, long d) {
    register long r10 __asm__("r10")=d;
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10):"rcx","r11","memory"); return r;
}
static inline long sc5(long n, long a, long b, long c, long d, long e) {
    register long r10 __asm__("r10")=d; register long r8 __asm__("r8")=e;
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8):"rcx","r11","memory"); return r;
}
static inline long sc6(long n, long a, long b, long c, long d, long e, long f) {
    register long r10 __asm__("r10")=d; register long r8 __asm__("r8")=e; register long r9 __asm__("r9")=f;
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory"); return r;
}

/* ── Syscall numbers ──────────────────────────── */

#define SYS_read           0
#define SYS_write          1
#define SYS_open           2
#define SYS_close          3
#define SYS_fstat          5
#define SYS_mmap           9
#define SYS_mprotect       10
#define SYS_munmap         11
#define SYS_brk            12
#define SYS_rt_sigaction   13
#define SYS_rt_sigprocmask 14
#define SYS_sched_yield    24
#define SYS_getpid         39
#define SYS_clone          56
#define SYS_fork           57
#define SYS_exit           60
#define SYS_wait4          61
#define SYS_kill           62
#define SYS_uname          63
#define SYS_getcwd         79
#define SYS_getpgrp        111
#define SYS_arch_prctl     158
#define SYS_gettid         186
#define SYS_clock_gettime  228
#define SYS_exit_group     231
#define SYS_getrandom      318

/* CosmoRT hardware syscalls */
#define SYS_COSMO_PCI_READ 516

/* ── Constants ────────────────────────────────── */

#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4
#define PROT_RW     0x3
#define MAP_PRIV_ANON 0x22
#define MAP_FIXED_NOREPLACE 0x100032
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR     2
#define O_CREAT    0x40
#define O_TRUNC    0x200
#define CLOCK_MONOTONIC 1
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define CLONE_VM    0x100
#define CLONE_THREAD 0x10000

#define SIGUSR1 10
#define SIGUSR2 12
#define SIGTERM 15

#define SA_RESTORER 0x04000000
#define SA_SIGINFO  0x00000004

/* ── Output ───────────────────────────────────── */

static inline void puts(const char *s) {
    int n = 0; while (s[n]) n++;
    sc3(SYS_write, 1, (long)s, n);
}

static inline void put_hex(uint64_t v) {
    char buf[17]; int i = 0;
    if (v == 0) { puts("0"); return; }
    while (v) { buf[i++] = "0123456789abcdef"[v & 0xf]; v >>= 4; }
    char out[17]; int j = 0;
    while (i--) out[j++] = buf[i];
    out[j] = 0;
    puts(out);
}

static inline void put_int(long v) {
    if (v < 0) { puts("-"); v = -v; }
    char buf[20]; int i = 0;
    do { buf[i++] = '0' + (char)(v % 10); v /= 10; } while (v);
    char out[20]; int j = 0;
    while (i--) out[j++] = buf[i];
    out[j] = 0;
    puts(out);
}

/* ── Test bookkeeping ─────────────────────────── */

extern int failures;
extern int passes;

static inline void pass(const char *name) {
    puts("  PASS  "); puts(name); puts("\n");
    passes++;
}

static inline void fail(const char *name, const char *detail) {
    puts("  FAIL  "); puts(name);
    if (detail) { puts(" ("); puts(detail); puts(")"); }
    puts("\n");
    failures++;
}

static inline void check(const char *name, int condition) {
    if (condition) pass(name); else fail(name, NULL);
}

static inline void check_val(const char *name, long got, long expected) {
    if (got == expected) {
        pass(name);
    } else {
        puts("  FAIL  "); puts(name);
        puts(" (got="); put_int(got);
        puts(" expected="); put_int(expected); puts(")\n");
        failures++;
    }
}

static inline void check_ge(const char *name, long got, long minimum) {
    if (got >= minimum) {
        pass(name);
    } else {
        puts("  FAIL  "); puts(name);
        puts(" (got="); put_int(got);
        puts(" min="); put_int(minimum); puts(")\n");
        failures++;
    }
}

#endif /* KTEST_H */
