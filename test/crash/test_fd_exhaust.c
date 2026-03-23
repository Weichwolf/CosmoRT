/* Crash test: FD table exhaustion.
 * Open pipes until EMFILE (24). Kernel must return EMFILE, not crash.
 * After cleanup, new FDs must work. */
#include "ktest.h"

#define ENOMEM 12
#define EMFILE 24
#define ENFILE 23
#define FD_LIMIT 300 /* more than FD_MAX (256) */

static void test_fd_exhaust(void) {
    puts("\n[FD Exhaust]\n");

    /* Count currently open FDs */
    int initially_open = 0;
    for (int fd = 0; fd < 64; fd++) {
        long r = sc2(SYS_FSTAT, fd, 0);
        if (r != -9) initially_open++; /* -9 = EBADF */
    }

    /* Phase 1: exhaust FDs via pipe2 (each call opens 2 FDs) */
    int pairs[FD_LIMIT][2];
    int opened = 0;
    int got_emfile = 0;

    for (int i = 0; i < FD_LIMIT; i++) {
        long r = sc2(293 /* SYS_PIPE2 */, (long)&pairs[i], 0);
        if (r < 0) {
            /* EMFILE (fd table full), ENFILE (system-wide), or ENOMEM
             * (pipe buffer allocation) are all valid resource exhaustion. */
            int expected = (r == -EMFILE || r == -ENFILE || r == -ENOMEM);
            puts("  pipe2 stopped at "); put_int(i);
            puts(": err="); put_int(r); puts("\n");
            got_emfile = expected;
            break;
        }
        opened++;
    }
    check("hit resource limit (EMFILE/ENFILE/ENOMEM)", got_emfile);
    puts("  pipe pairs opened: "); put_int(opened); puts("\n");

    /* Phase 2: verify additional opens fail */
    int extra_pair[2];
    long r = sc2(293, (long)&extra_pair, 0);
    check("extra pipe2 after exhaustion fails", r < 0);

    /* Phase 3: close all FDs */
    for (int i = 0; i < opened; i++) {
        sc1(SYS_CLOSE, pairs[i][0]);
        sc1(SYS_CLOSE, pairs[i][1]);
    }

    /* Phase 4: verify we can open new FDs after cleanup */
    int check_pair[2];
    r = sc2(293, (long)&check_pair, 0);
    check("pipe2 after cleanup works", r == 0);
    if (r == 0) {
        sc1(SYS_CLOSE, check_pair[0]);
        sc1(SYS_CLOSE, check_pair[1]);
    }

    /* Phase 5: verify no FD leak (same count as before) */
    int final_open = 0;
    for (int fd = 0; fd < 64; fd++) {
        long r2 = sc2(SYS_FSTAT, fd, 0);
        if (r2 != -9) final_open++;
    }
    check("no fd leak", final_open == initially_open);
}

CRASH_TEST("crash/fd_exhaust", test_fd_exhaust);
