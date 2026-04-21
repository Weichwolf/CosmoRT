/* Class A: TID-Tabellen-Wachstum.
 *
 * CLONE_THREAD spawnt in selbe task-Group. Initial 256 Slots → Growth ab 257.
 *
 * Threads werden nie zurueck-gejoined (CosmoRT hat kein natives pthread_join
 * API in ktest) — wir pruefen nur den Growth-Pfad: TID-Zuweisung scheitert nicht,
 * keine TID-Kollision, Prozess kommt nicht durcheinander. */
#include "ktest.h"

#define THREAD_COUNT      260
#define THREAD_STACK_SZ   16384

#define MSEC_BURST        20
#define SPIN_MS_ITERS     500000

static volatile int started_count;
static volatile int exit_gate;

__attribute__((noreturn)) static void worker(void) {
    __sync_fetch_and_add(&started_count, 1);
    while (__sync_fetch_and_add(&exit_gate, 0) == 0)
        __asm__ volatile("pause");
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static void test_tid_table_growth(void) {
    puts("\n[TID_TABLE_GROWTH]\n");

    started_count = 0;
    exit_gate = 0;

    long stack_region = sc6(SYS_MMAP, 0,
                            (long)THREAD_COUNT * THREAD_STACK_SZ,
                            PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap stacks", stack_region > 0);
    if (stack_region <= 0) return;

    static long tids[THREAD_COUNT];
    int spawned = 0;
    for (int i = 0; i < THREAD_COUNT; i++) {
        long stk = stack_region + (long)i * THREAD_STACK_SZ + THREAD_STACK_SZ;
        long r = sc5(SYS_CLONE,
                     CLONE_VM | CLONE_SIGHAND | CLONE_THREAD,
                     stk, 0, 0, 0);
        if (r == 0) worker();
        if (r < 0) break;
        tids[spawned++] = r;
    }

    check_ge("spawned > 256 threads", spawned, 257);

    int dup = 0;
    for (int i = 0; i < spawned; i++)
        for (int j = i + 1; j < spawned; j++)
            if (tids[i] == tids[j]) dup++;
    check_val("no duplicate tids across growth", dup, 0);

    for (volatile int w = 0; w < SPIN_MS_ITERS * 4; w++) {
        if (started_count >= spawned) break;
        __asm__ volatile("pause");
    }
    check_ge("workers reached run state", (long)started_count, (long)(spawned * 3 / 4));

    __sync_fetch_and_add(&exit_gate, 1);
    for (volatile int w = 0; w < SPIN_MS_ITERS * 8; w++) __asm__ volatile("pause");

    sc2(SYS_MUNMAP, stack_region, (long)THREAD_COUNT * THREAD_STACK_SZ);
}

TEST("tid/table_growth", test_tid_table_growth);
