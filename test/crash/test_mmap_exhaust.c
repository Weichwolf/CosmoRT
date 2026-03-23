/* Crash test: mmap exhaustion.
 * mmap in a loop until ENOMEM. Kernel must survive, parent can still fork/exec. */
#include "ktest.h"

#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFEXITED(s)    (((s) & 0x7F) == 0)

#define MB (1024ULL * 1024)
#define CHUNK (16 * MB)
#define MAX_ALLOCS 2048

static void test_mmap_exhaust(void) {
    puts("\n[Mmap Exhaust]\n");

    /* Phase 1: exhaust mmap in child */
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        int count = 0;
        for (int i = 0; i < MAX_ALLOCS; i++) {
            long r = sc6(SYS_MMAP, 0, (long)CHUNK, PROT_RW,
                         MAP_PRIV_ANON, -1, 0);
            if (r < 0) {
                /* Must be -ENOMEM */
                if (r != -12) {
                    puts("  unexpected error: "); put_int(r); puts("\n");
                    sc1(SYS_EXIT_GROUP, 2);
                }
                sc1(SYS_EXIT_GROUP, 0); /* success: got ENOMEM */
            }
            /* Touch to commit */
            *(volatile char *)r = 1;
            count++;
        }
        /* Didn't hit ENOMEM — still ok, just means lots of memory */
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }
    check("fork for exhaust", pid > 0);

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("exhaust child exited cleanly",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* Phase 2: kernel survived — verify parent can still fork/exec */
    pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Child: allocate a page, touch it, exit */
        long pg = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
        if (pg < 0)
            sc1(SYS_EXIT_GROUP, 1);
        *(volatile char *)pg = 42;
        sc2(SYS_MUNMAP, pg, 4096);
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }
    check("fork after exhaust", pid > 0);

    status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("post-exhaust child ok",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* Phase 3: parent can still mmap */
    long pg = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("parent mmap after exhaust", pg > 0);
    if (pg > 0) {
        *(volatile char *)pg = 1;
        sc2(SYS_MUNMAP, pg, 4096);
    }
}

CRASH_TEST("crash/mmap_exhaust", test_mmap_exhaust);
