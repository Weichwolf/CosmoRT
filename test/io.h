/* Freestanding output helpers for ktest */
#ifndef IO_H
#define IO_H

#include "syscall.h"

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

/* Direct serial output via SYS_WRITE fd=2 (stderr → serial).
 * Used by the parallel test runner parent to avoid interleaving
 * with child stdout output. */
static inline void puts_direct(const char *s) {
    int n = 0; while (s[n]) n++;
    sc3(SYS_WRITE, 2, (long)s, n);
}

static inline void put_int_direct(long v) {
    if (v < 0) { puts_direct("-"); v = -v; }
    char buf[20]; int i = 0;
    do { buf[i++] = '0' + (char)(v % 10); v /= 10; } while (v);
    char out[20]; int j = 0;
    while (i--) out[j++] = buf[i];
    out[j] = 0;
    puts_direct(out);
}

/* Shared memory slot for parallel test runner IPC */
typedef struct {
    volatile int done;      /* 1 = child finished */
    volatile int pass_cnt;  /* passes in child */
    volatile int fail_cnt;  /* failures in child */
    volatile int exit_code; /* child exit status */
    volatile long pid;      /* child PID */
} test_slot_t;

#endif
