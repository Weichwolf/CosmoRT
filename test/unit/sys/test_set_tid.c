#include "ktest.h"

static void test_set_tid(void) {
    puts("\n[set_tid_address]\n");

    /* Test 1: set_tid_address returns current TID */
    int tid_var = -1;
    long tid = sc1(SYS_SET_TID_ADDRESS, (long)&tid_var);
    long actual_tid = sc0(SYS_GETTID);
    check_val("set_tid_address returns TID", tid, actual_tid);

    /* Test 2: clone thread with CLONE_CHILD_CLEARTID, tid_var cleared on exit */
    volatile int child_tid = -1;
    long stack = sc6(SYS_MMAP, 0, 65536, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap thread stack", stack > 0);
    if (stack <= 0) return;

    long ret = sc5(SYS_CLONE,
                   CLONE_VM | CLONE_SIGHAND | CLONE_THREAD | CLONE_CHILD_CLEARTID,
                   stack + 65536,
                   0,  /* parent_tid */
                   (long)&child_tid,  /* child_tid */
                   0);
    if (ret == 0) {
        /* Child thread: exit immediately */
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }
    check("clone returns tid", ret > 0);

    /* Wait for child thread to exit and clear tid_var via futex_wake.
     * Poll with timeout — child_tid should be set to 0 by kernel. */
    for (volatile int i = 0; i < 10000000 && child_tid != 0; i++)
        __asm__ volatile("pause");

    check_val("child_tid cleared to 0", child_tid, 0);
}

TEST("set_tid", test_set_tid);
