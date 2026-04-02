#include "ktest.h"

static volatile int worker_done = 0;

static void worker_fn(void) {
    __sync_fetch_and_add(&worker_done, 1);
    for (;;) __asm__ volatile("pause");
}

static void test_threads(void) {
    puts("\n[Threads]\n");

    worker_done = 0;

    /* Allocate stack for worker */
    long stack = sc6(SYS_MMAP, 0, 65536, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap thread stack", stack > 0);
    if (stack <= 0) return;

    long ret = sc5(SYS_CLONE, CLONE_VM | CLONE_SIGHAND | CLONE_THREAD, stack + 65536, 0, 0, 0);
    if (ret == 0) {
        /* Child */
        worker_fn();
        __builtin_unreachable();
    }
    check("clone returns tid", ret > 0);

    /* Wait for worker to signal completion */
    for (volatile int i = 0; i < 10000000 && !worker_done; i++)
        __asm__ volatile("pause");

    check("worker thread ran", worker_done > 0);

    puts("  worker tid="); put_int(ret); puts("\n");
}

TEST("threads", test_threads);
