/* CosmoRT ktest — test framework */
#ifndef KTEST_H
#define KTEST_H

#include "syscall.h"
#include "io.h"

/* Shorthand */
#define PROT_RW     (PROT_READ | PROT_WRITE)
#define MAP_PRIV_ANON (MAP_PRIVATE | MAP_ANONYMOUS)

/* ── Assertions ──────────────────────────────── */

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
    if (condition) pass(name); else fail(name, 0);
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

/* ── Self-registering tests ──────────────────── */

typedef struct {
    const char *name;
    void (*fn)(void);
    int crash;
    int _pad;
    uint64_t _reserved;
} ktest_entry_t;

#define TEST(n, fn) \
    __attribute__((used, section(".ktest"))) \
    static const ktest_entry_t _reg_##fn = { n, fn, 0 }

#define CRASH_TEST(n, fn) \
    __attribute__((used, section(".ktest"))) \
    static const ktest_entry_t _reg_##fn = { n, fn, 1 }

#endif
