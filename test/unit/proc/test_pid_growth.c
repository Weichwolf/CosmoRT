/* Class A: PID-Tabellen-Wachstum.
 *
 * Initial 256 Slots, Verdopplung on-demand bis PID_MAX_CEILING=2^22.
 * Live-Prozesse > 256 zwingt Growth-Pfad.
 *
 * Child-Watchdog: jedes Kind setzt alarm(3) → SIGALRM tot, falls Parent stirbt
 * bevor gate gesetzt wird. Verhindert Zombie-Armee beim Test-Watchdog-Kill. */
#include "ktest.h"

#define PARALLEL_PROCS  260
#define CHILD_ALARM_SEC 3

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

static void test_pid_table_growth(void) {
    puts("\n[PID_TABLE_GROWTH]\n");

    volatile int *gate = (volatile int *)sc6(SYS_MMAP, 0, 4096,
        PROT_RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check("gate page", (long)gate > 0);
    if ((long)gate <= 0) return;
    *gate = 0;

    long pids[PARALLEL_PROCS];
    int alive = 0;
    for (int i = 0; i < PARALLEL_PROCS; i++) {
        long pid = sc0(SYS_FORK);
        if (pid < 0) break;
        if (pid == 0) {
            struct k_sigaction dfl = { 0, 0, 0, 0 };
            sc4(SYS_RT_SIGACTION, SIGALRM, (long)&dfl, 0, 8);
            sc1(SYS_ALARM, CHILD_ALARM_SEC);
            int spins = 0;
            while (*gate == 0 && spins < 200000) {
                for (volatile int j = 0; j < 1000; j++) __asm__ volatile("pause");
                spins++;
            }
            sc1(SYS_EXIT_GROUP, 0);
            __builtin_unreachable();
        }
        pids[alive++] = pid;
    }

    check_ge("spawned > 256 live procs", alive, 257);

    long max_pid = 0;
    for (int i = 0; i < alive; i++)
        if (pids[i] > max_pid) max_pid = pids[i];
    puts("  alive="); put_int(alive);
    puts(" max_pid="); put_int(max_pid); puts("\n");

    *gate = 1;

    int reaped = 0;
    for (int i = 0; i < alive; i++) {
        int status = 0;
        long w = sc4(SYS_WAIT4, pids[i], (long)&status, 0, 0);
        if (w == pids[i] && WIFEXITED(status) && WEXITSTATUS(status) == 0)
            reaped++;
    }
    check_val("all children reaped after growth", reaped, alive);
    sc2(SYS_MUNMAP, (long)gate, 4096);
}

static void test_pid_unique_after_growth(void) {
    puts("\n[PID_UNIQUE_GROWTH]\n");

    volatile int *gate = (volatile int *)sc6(SYS_MMAP, 0, 4096,
        PROT_RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if ((long)gate <= 0) { check("mmap gate", 0); return; }
    *gate = 0;

    long pids[PARALLEL_PROCS];
    int alive = 0;
    for (int i = 0; i < PARALLEL_PROCS; i++) {
        long pid = sc0(SYS_FORK);
        if (pid < 0) break;
        if (pid == 0) {
            sc1(SYS_ALARM, CHILD_ALARM_SEC);
            while (*gate == 0)
                for (volatile int j = 0; j < 1000; j++) __asm__ volatile("pause");
            sc1(SYS_EXIT_GROUP, 0);
            __builtin_unreachable();
        }
        pids[alive++] = pid;
    }

    int dup = 0;
    for (int i = 0; i < alive; i++)
        for (int j = i + 1; j < alive; j++)
            if (pids[i] == pids[j]) dup++;
    check_val("no duplicate pids across growth", dup, 0);

    *gate = 1;
    for (int i = 0; i < alive; i++)
        sc4(SYS_WAIT4, pids[i], 0, 0, 0);
    sc2(SYS_MUNMAP, (long)gate, 4096);
}

TEST("pid/table_growth",  test_pid_table_growth);
TEST("pid/unique_growth", test_pid_unique_after_growth);
