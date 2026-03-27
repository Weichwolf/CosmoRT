/* Crash test: W^X enforcement.
 * mmap/mprotect with PROT_WRITE|PROT_EXEC must return -EINVAL. */
#include "ktest.h"

#define EINVAL 22

static void test_wxe(void) {
    puts("\n[W^X Enforcement]\n");

    /* mmap(RWX) must fail */
    long r = sc6(SYS_MMAP, 0, 4096,
                 PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIV_ANON, -1, 0);
    check_val("mmap(RWX) → -EINVAL", r, -EINVAL);

    /* mmap(WX) must fail */
    r = sc6(SYS_MMAP, 0, 4096,
            PROT_WRITE | PROT_EXEC,
            MAP_PRIV_ANON, -1, 0);
    check_val("mmap(WX) → -EINVAL", r, -EINVAL);

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

    /* mprotect(RW→RWX) must fail */
    if (rw_addr > 0) {
        r = sc3(SYS_MPROTECT, rw_addr, 4096,
                PROT_READ | PROT_WRITE | PROT_EXEC);
        check_val("mprotect(RW→RWX) → -EINVAL", r, -EINVAL);

        /* mprotect(RW→RX) must succeed */
        r = sc3(SYS_MPROTECT, rw_addr, 4096,
                PROT_READ | PROT_EXEC);
        check_val("mprotect(RW→RX) → 0", r, 0);

        sc2(SYS_MUNMAP, rw_addr, 4096);
    }
}

CRASH_TEST("crash/wxe", test_wxe);
