#include "ktest.h"

static void test_yield(void) {
    puts("\n[Scheduler]\n");
    long r = sc0(SYS_sched_yield);
    check_val("sched_yield returns 0", r, 0);
}

TEST("sched", test_yield);
