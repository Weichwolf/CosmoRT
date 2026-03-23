/* CosmoRT ktest — shared definitions for hardware tests */
#ifndef KTEST_H
#define KTEST_H

/* All constants (syscall numbers, errno, flags, structs) from uapi */
#include "cosmo_uapi.h"

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

/* ── Additional constants used by tests ─────── */

#define PROT_RW     (PROT_READ | PROT_WRITE)
#define MAP_PRIV_ANON (MAP_PRIVATE | MAP_ANONYMOUS)

/* ── Output ───────────────────────────────────── */

static inline void puts(const char *s) {
    int n = 0; while (s[n]) n++;
    sc3(SYS_WRITE, 1, (long)s, n);
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

/* ── Self-registering tests via linker section ── */

typedef struct {
    const char *name;
    void (*fn)(void);
    int crash;  /* 1 = run in fork'd child */
    int _pad;
    uint64_t _reserved;
} ktest_entry_t;

#define TEST(n, fn) \
    __attribute__((used, section(".ktest"))) \
    static const ktest_entry_t _reg_##fn = { n, fn, 0 }

#define CRASH_TEST(n, fn) \
    __attribute__((used, section(".ktest"))) \
    static const ktest_entry_t _reg_##fn = { n, fn, 1 }

#endif /* KTEST_H */
