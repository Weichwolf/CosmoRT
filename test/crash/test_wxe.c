/* Crash test: mmap/mprotect protection flags.
 * RWX is allowed (required by JIT engines like V8). */
#include "ktest.h"

static void test_wxe(void) {
    puts("\n[Memory Protection Flags]\n");

    /* mmap(RWX) must succeed (JIT needs this) */
    long r = sc6(SYS_MMAP, 0, 4096,
                 PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIV_ANON, -1, 0);
    check("mmap(RWX) succeeds", r > 0);
    if (r > 0) sc2(SYS_MUNMAP, r, 4096);

    /* mmap(WX) must succeed */
    r = sc6(SYS_MMAP, 0, 4096,
            PROT_WRITE | PROT_EXEC,
            MAP_PRIV_ANON, -1, 0);
    check("mmap(WX) succeeds", r > 0);
    if (r > 0) sc2(SYS_MUNMAP, r, 4096);

    /* mmap(RX) must succeed */
    r = sc6(SYS_MMAP, 0, 4096,
            PROT_READ | PROT_EXEC,
            MAP_PRIV_ANON, -1, 0);
    check("mmap(RX) succeeds", r > 0);
    if (r > 0) sc2(SYS_MUNMAP, r, 4096);

    /* mmap(RW) must succeed */
    r = sc6(SYS_MMAP, 0, 4096,
            PROT_READ | PROT_WRITE,
            MAP_PRIV_ANON, -1, 0);
    check("mmap(RW) succeeds", r > 0);
    long rw_addr = r;

    /* mprotect(RW→RWX) must succeed (V8 JIT pattern) */
    if (rw_addr > 0) {
        r = sc3(SYS_MPROTECT, rw_addr, 4096,
                PROT_READ | PROT_WRITE | PROT_EXEC);
        check_val("mprotect(RW→RWX) → 0", r, 0);

        /* mprotect(RWX→RX) must succeed */
        r = sc3(SYS_MPROTECT, rw_addr, 4096,
                PROT_READ | PROT_EXEC);
        check_val("mprotect(RWX→RX) → 0", r, 0);

        sc2(SYS_MUNMAP, rw_addr, 4096);
    }
}

CRASH_TEST("crash/wxe", test_wxe);
