/* Crash test: Fork bomb stress.
 * 100 fork/wait cycles. Kernel must not leak resources. */
#include "ktest.h"

#define FORK_CYCLES 100

static void test_fork_bomb(void) {
    puts("\n[Fork Bomb]\n");

    /* Snapshot: count open fds before */
    int fds_before = 0;
    for (int fd = 0; fd < 64; fd++) {
        long r = sc2(SYS_FSTAT, fd, 0); /* just probe, buf=NULL will EFAULT but not EBADF */
        if (r != -9) fds_before++; /* -9 = EBADF → not open */
    }

    /* mmap a page to detect leaks via memory pressure */
    long sentinel = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("sentinel page", sentinel > 0);
    *(volatile uint64_t *)sentinel = 0xFEEDFACE;

    int ok = 0;
    for (int i = 0; i < FORK_CYCLES; i++) {
        long pid = sc0(SYS_FORK);
        if (pid < 0) {
            puts("  fork failed at cycle "); put_int(i);
            puts(" (err="); put_int(pid); puts(")\n");
            break;
        }
        if (pid == 0) {
            /* Child: exit immediately */
            sc1(SYS_EXIT_GROUP, 0);
            __builtin_unreachable();
        }
        /* Parent: wait for child */
        int status = 0;
        long w = sc4(SYS_WAIT4, (long)pid, (long)&status, 0, 0);
        if (w < 0) {
            puts("  wait4 failed at cycle "); put_int(i);
            puts(" (err="); put_int(w); puts(")\n");
            break;
        }
        ok++;
    }
    check("fork/wait 100 cycles", ok == FORK_CYCLES);

    /* Verify sentinel intact (no memory corruption) */
    check("sentinel intact", *(volatile uint64_t *)sentinel == 0xFEEDFACE);

    /* Check fd count unchanged */
    int fds_after = 0;
    for (int fd = 0; fd < 64; fd++) {
        long r = sc2(SYS_FSTAT, fd, 0);
        if (r != -9) fds_after++;
    }
    check("no fd leak", fds_after == fds_before);

    sc2(SYS_MUNMAP, sentinel, 4096);

    /* Rapid-fire: fork many without waiting, then reap all */
    long pids[32];
    int spawned = 0;
    for (int i = 0; i < 32; i++) {
        long pid = sc0(SYS_FORK);
        if (pid < 0) break;
        if (pid == 0) {
            sc1(SYS_EXIT_GROUP, 0);
            __builtin_unreachable();
        }
        pids[i] = pid;
        spawned++;
    }
    int reaped = 0;
    for (int i = 0; i < spawned; i++) {
        long w = sc4(SYS_WAIT4, pids[i], 0, 0, 0);
        if (w > 0) reaped++;
    }
    check("rapid fork+reap", reaped == spawned);
    puts("  rapid: spawned="); put_int(spawned);
    puts(" reaped="); put_int(reaped); puts("\n");
}

CRASH_TEST("crash/fork_bomb", test_fork_bomb);
