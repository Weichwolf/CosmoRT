/* Class E: Korruptions-Detection / Race-Conditions.
 *
 * Close-Race auf geteilten fd, PID-Reuse, parallel fork + mmap. */
#include "ktest.h"

#define CLOSE_RACE_ITERS    32
#define PID_REUSE_CYCLES    5

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

static void test_close_race_shared_fd(void) {
    puts("\n[CLOSE_RACE_SHARED_FD]\n");

    int ok_races = 0;
    for (int iter = 0; iter < CLOSE_RACE_ITERS; iter++) {
        int fds[2];
        if (sc2(SYS_PIPE2, (long)fds, 0) != 0) break;

        long pid = sc0(SYS_FORK);
        if (pid < 0) { sc1(SYS_CLOSE, fds[0]); sc1(SYS_CLOSE, fds[1]); break; }

        if (pid == 0) {
            for (volatile int k = 0; k < 500; k++) __asm__ volatile("pause");
            sc1(SYS_CLOSE, fds[0]);
            sc1(SYS_CLOSE, fds[1]);
            sc1(SYS_EXIT_GROUP, 0);
            __builtin_unreachable();
        }

        sc1(SYS_CLOSE, fds[0]);
        sc1(SYS_CLOSE, fds[1]);

        int status = 0;
        sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
        if (WIFEXITED(status)) ok_races++;
    }
    check_val("no crash on close race", ok_races, CLOSE_RACE_ITERS);

    long probe = sc2(SYS_PIPE2, (long)(long[]){0,0}, 0);
    (void)probe;
    int p2[2];
    long r = sc2(SYS_PIPE2, (long)p2, 0);
    check_val("pipe2 still works post-race", r, 0);
    if (r == 0) { sc1(SYS_CLOSE, p2[0]); sc1(SYS_CLOSE, p2[1]); }
}

static void test_parallel_fork_mmap(void) {
    puts("\n[PARALLEL_FORK_MMAP]\n");

    long base = sc6(SYS_MMAP, 0, 4 * 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("parent mmap", base > 0);
    if (base <= 0) return;

    for (int i = 0; i < 4; i++) {
        uint64_t *p = (uint64_t *)(base + (long)i * 4096);
        *p = 0xAAAAAA00ULL | (uint64_t)i;
    }

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) { sc2(SYS_MUNMAP, base, 4 * 4096); return; }

    if (pid == 0) {
        int match = 0;
        for (int i = 0; i < 4; i++) {
            uint64_t v = *(volatile uint64_t *)(base + (long)i * 4096);
            if (v == (0xAAAAAA00ULL | (uint64_t)i)) match++;
        }
        long extra = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
        if (extra <= 0) sc1(SYS_EXIT_GROUP, 10);
        *(volatile uint64_t *)extra = 0xBBBBBBBB;
        int code = (match == 4 && *(volatile uint64_t *)extra == 0xBBBBBBBBULL) ? 0 : 50;
        sc2(SYS_MUNMAP, extra, 4096);
        sc1(SYS_EXIT_GROUP, code);
        __builtin_unreachable();
    }

    int after_match = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t v = *(volatile uint64_t *)(base + (long)i * 4096);
        if (v == (0xAAAAAA00ULL | (uint64_t)i)) after_match++;
    }
    check_val("parent state intact after fork", after_match, 4);

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check_val("child saw shared state", (long)WEXITSTATUS(status), 0);

    sc2(SYS_MUNMAP, base, 4 * 4096);
}

static void test_pid_reuse_delay(void) {
    puts("\n[PID_REUSE_DELAY]\n");

    long first_pid = sc0(SYS_FORK);
    check("first fork", first_pid >= 0);
    if (first_pid < 0) return;
    if (first_pid == 0) {
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }
    sc4(SYS_WAIT4, first_pid, 0, 0, 0);

    int reused = 0;
    long subsequent[PID_REUSE_CYCLES];
    for (int i = 0; i < PID_REUSE_CYCLES; i++) {
        long pid = sc0(SYS_FORK);
        if (pid < 0) { subsequent[i] = 0; continue; }
        if (pid == 0) {
            sc1(SYS_EXIT_GROUP, 0);
            __builtin_unreachable();
        }
        subsequent[i] = pid;
        if (pid == first_pid) reused++;
    }
    check_val("no immediate PID reuse", reused, 0);

    for (int i = 0; i < PID_REUSE_CYCLES; i++)
        if (subsequent[i] > 0) sc4(SYS_WAIT4, subsequent[i], 0, 0, 0);
}

TEST("race/close_shared_fd",  test_close_race_shared_fd);
TEST("race/parallel_fork_mm", test_parallel_fork_mmap);
TEST("race/pid_reuse",        test_pid_reuse_delay);
