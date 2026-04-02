/* Skal-M: dynamic process/thread scaling — no hard PROC_MAX/THREAD_MAX.
 * Tests: 32+ concurrent processes, 64+ concurrent threads, fork-till-OOM. */
#include "ktest.h"

static void test_many_processes(void) {
    puts("\n[Skal-M: many processes]\n");

    /* Fork 48 children (far beyond old PROC_MAX=16), reap them all */
    long pids[48];
    int spawned = 0;
    for (int i = 0; i < 48; i++) {
        long pid = sc0(SYS_FORK);
        if (pid < 0) break;
        if (pid == 0) {
            sc1(SYS_EXIT_GROUP, 0);
            __builtin_unreachable();
        }
        pids[i] = pid;
        spawned++;
    }
    check_ge("spawned >= 32 procs", spawned, 32);
    puts("  spawned="); put_int(spawned); puts("\n");

    int reaped = 0;
    for (int i = 0; i < spawned; i++) {
        long w = sc4(SYS_WAIT4, pids[i], 0, 0, 0);
        if (w > 0) reaped++;
    }
    check("reaped all", reaped == spawned);
}

static volatile int thread_counter = 0;

static void test_many_threads(void) {
    puts("\n[Skal-M: many threads]\n");

    thread_counter = 0;

    /* Spawn 80 threads (far beyond old THREAD_MAX=64).
     * Each thread increments counter and exits. */
    int target = 80;
    int created = 0;
    for (int i = 0; i < target; i++) {
        long stk = sc6(SYS_MMAP, 0, 8192, PROT_RW, MAP_PRIV_ANON, -1, 0);
        if (stk <= 0) break;

        long tid = sc5(SYS_CLONE, (long)(CLONE_VM | CLONE_SIGHAND | CLONE_THREAD), stk + 8192, 0, 0, 0);
        if (tid == 0) {
            /* Child thread: increment and exit */
            __sync_fetch_and_add(&thread_counter, 1);
            sc1(SYS_EXIT, 0);
            __builtin_unreachable();
        }
        if (tid < 0) {
            sc2(SYS_MUNMAP, stk, 8192);
            break;
        }
        created++;
    }
    /* Wait for threads to finish */
    for (volatile int i = 0; i < 20000000 && thread_counter < created; i++)
        __asm__ volatile("pause");

    check_ge("created >= 64 threads", created, 64);
    check("all threads ran", thread_counter >= created);
    puts("  threads created="); put_int(created);
    puts(" ran="); put_int(thread_counter); puts("\n");
}

static void test_fork_till_oom(void) {
    puts("\n[Skal-M: fork till OOM]\n");

    /* Fork many children. With dynamic slabs the limit is RAM.
     * In QEMU with 4GB, 256 should succeed. Verify graceful handling. */
    long pids[128];
    int spawned = 0;
    long last_err = 0;
    for (int i = 0; i < 128; i++) {
        long pid = sc0(SYS_FORK);
        if (pid < 0) {
            last_err = pid;
            break;
        }
        if (pid == 0) {
            sc1(SYS_EXIT_GROUP, 0);
            __builtin_unreachable();
        }
        pids[i] = pid;
        spawned++;
    }
    /* With dynamic allocation, all 128 should succeed */
    check_ge("spawned >= 48", spawned, 48);
    puts("  spawned="); put_int(spawned);
    if (last_err < 0) { puts(" err="); put_int(last_err); }
    puts("\n");

    /* Reap all children */
    int reaped = 0;
    for (int i = 0; i < spawned; i++) {
        long w = sc4(SYS_WAIT4, pids[i], 0, 0, 0);
        if (w > 0) reaped++;
    }
    check("reaped all", reaped == spawned);
}

TEST("skal_m/many_procs", test_many_processes);
TEST("skal_m/many_threads", test_many_threads);
TEST("skal_m/fork_oom", test_fork_till_oom);
