#include "ktest.h"

static void test_clone_flags(void) {
    puts("\n[clone flags]\n");

    /* Allocate child stack */
    long stk = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap clone stack", stk > 0);
    if (stk <= 0) return;

    /* clone with CLONE_VM + all thread flags should succeed */
    unsigned long fl = CLONE_VM | CLONE_FS | CLONE_FILES |
                       CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM;
    long tid = sc5(SYS_CLONE, (long)fl, stk + 4096, 0, 0, 0);
    if (tid == 0) {
        /* child: exit immediately */
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }
    check("clone(VM|FS|FILES|SIGHAND|SYSVSEM) tid > 0", tid > 0);

    /* clone without CLONE_VM should fail (CosmoRT: threads only via clone) */
    long r = sc5(SYS_CLONE, (long)(CLONE_FS | CLONE_FILES), stk + 4096, 0, 0, 0);
    check_val("clone without CLONE_VM = -EINVAL", r, -EINVAL);

    sc2(SYS_MUNMAP, stk, 4096);
}

TEST("clone_flags", test_clone_flags);
