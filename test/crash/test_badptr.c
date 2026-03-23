/* Crash test: Invalid pointers to syscalls.
 * Every syscall that takes a pointer must reject kernel addresses. */
#include "ktest.h"

#define EFAULT 14
#define EINVAL 22

/* Syscall numbers not in ktest.h */
#define SYS_PIPE   22
#define SYS_OPENAT 257

/* Kernel-space addresses */
#define KADDR_HIGH  0xFFFF800000000000ULL
#define KADDR_CANON 0xFFFFFFFF80000000ULL
#define ADDR_NULL   0ULL
#define ADDR_DEAD   0xDEAD000000000000ULL
#define ADDR_LOW    0x1ULL  /* unmapped low page */

static void check_efault(const char *name, long result) {
    if (result == -EFAULT) {
        pass(name);
    } else {
        puts("  FAIL  "); puts(name);
        puts(" (got="); put_int(result);
        puts(" expected=-"); put_int(EFAULT); puts(")\n");
        failures++;
    }
}

/* Must return -EFAULT or -EINVAL, anything but success or crash */
static void check_rejected(const char *name, long result) {
    if (result < 0) {
        pass(name);
    } else {
        puts("  FAIL  "); puts(name);
        puts(" (got="); put_int(result); puts(" expected negative)\n");
        failures++;
    }
}

static void test_badptr(void) {
    puts("\n[Bad Pointers]\n");

    /* ── read() ── */
    check_efault("read(0, kaddr_high, 100)",
        sc3(SYS_READ, 0, (long)KADDR_HIGH, 100));
    check_efault("read(0, kaddr_canon, 100)",
        sc3(SYS_READ, 0, (long)KADDR_CANON, 100));

    /* ── write() ── */
    check_efault("write(1, NULL, 100)",
        sc3(SYS_WRITE, 1, (long)ADDR_NULL, 100));
    check_efault("write(1, kaddr_high, 100)",
        sc3(SYS_WRITE, 1, (long)KADDR_HIGH, 100));

    /* ── open() with bad path ── */
    check_efault("open(kaddr_high, 0, 0)",
        sc3(SYS_OPEN, (long)KADDR_HIGH, 0, 0));
    check_efault("open(0xDEAD, 0, 0)",
        sc3(SYS_OPEN, (long)ADDR_DEAD, 0, 0));

    /* ── mmap with kernel address hint ── */
    check_rejected("mmap(kaddr, 4096, RW)",
        sc6(SYS_MMAP, (long)KADDR_HIGH, 4096, PROT_RW,
            MAP_PRIV_ANON, -1, 0));

    /* ── mprotect on kernel address ── */
    check_rejected("mprotect(kaddr, 4096, RW)",
        sc3(SYS_MPROTECT, (long)KADDR_HIGH, 4096, PROT_RW));

    /* ── munmap kernel address ── */
    check_rejected("munmap(kaddr, 4096)",
        sc2(SYS_MUNMAP, (long)KADDR_HIGH, 4096));

    /* ── clock_gettime with bad buffer ── */
    check_efault("clock_gettime(0, kaddr)",
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)KADDR_HIGH));
    check_efault("clock_gettime(0, NULL)",
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)ADDR_NULL));

    /* ── uname with bad buffer ── */
    check_efault("uname(kaddr)",
        sc1(SYS_UNAME, (long)KADDR_HIGH));

    /* ── getrandom with bad buffer ── */
    check_efault("getrandom(kaddr, 8, 0)",
        sc3(SYS_GETRANDOM, (long)KADDR_HIGH, 8, 0));

    /* ── getcwd with bad buffer ── */
    check_efault("getcwd(kaddr, 256)",
        sc2(SYS_GETCWD, (long)KADDR_HIGH, 256));

    /* ── rt_sigaction with bad pointers ── */
    check_efault("rt_sigaction(10, kaddr, 0, 8)",
        sc4(SYS_RT_SIGACTION, 10, (long)KADDR_HIGH, 0, 8));

    /* ── fstat with bad buffer ── */
    check_efault("fstat(1, kaddr)",
        sc2(SYS_FSTAT, 1, (long)KADDR_HIGH));

    /* ── pipe with bad buffer ── */
    check_efault("pipe(kaddr)",
        sc1(SYS_PIPE, (long)KADDR_HIGH));

    /* ── Wrap-around address: just below boundary ── */
    /* size that wraps past kernel space */
    check_efault("read(0, 0x7FFFFFFFE000, 0x10000)",
        sc3(SYS_READ, 0, 0x7FFFFFFFE000LL, 0x10000));

    /* ── write(1, low_unmapped, 1) — should not crash ── */
    long r = sc3(SYS_WRITE, 1, (long)ADDR_LOW, 1);
    check("write(1, 0x1, 1) rejected", r < 0);

    /* ── Bonus: huge size values ── */
    check_rejected("mmap(0, SIZE_MAX, RW)",
        sc6(SYS_MMAP, 0, (long)-1, PROT_RW, MAP_PRIV_ANON, -1, 0));
    check_rejected("mmap(0, -4096, RW)",
        sc6(SYS_MMAP, 0, (long)-4096, PROT_RW, MAP_PRIV_ANON, -1, 0));

    /* ── brk to kernel address ── */
    long brk_orig = sc1(SYS_BRK, 0);
    long brk_bad = sc1(SYS_BRK, (long)KADDR_HIGH);
    check("brk(kaddr) rejected", brk_bad == brk_orig || brk_bad < (long)KADDR_HIGH);

    /* ── openat with bad path ── */
    long oat = sc4(SYS_OPENAT, -100 /* AT_FDCWD */, (long)KADDR_HIGH, 0, 0);
    check("openat(-100, kaddr) → EFAULT", oat == -EFAULT);
}

CRASH_TEST("crash/badptr", test_badptr);
