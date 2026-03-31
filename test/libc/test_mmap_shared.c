/* test: MAP_SHARED anonymous mmap — reproduces musl sem_open failure */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

/*
 * MAP_SHARED|MAP_ANONYMOUS pages must be visible across fork.
 * musl sem_open uses shm_open + mmap(MAP_SHARED). If coherence is broken,
 * sem_getvalue returns wrong initial value.
 * CosmoRT: parent still sees 42 after child writes 99 (separate pages).
 */
static void test_mmap_shared(void) {
    puts("\n[mm/mmap-shared]\n");

    long page = sc6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check("mmap shared", page > 0);
    if (page < 0) return;

    volatile int *val = (volatile int *)page;
    *val = 42;

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) { sc2(SYS_MUNMAP, page, 4096); return; }

    if (pid == 0) {
        /* child: write 99 to shared page */
        *val = 99;
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    /* parent: wait for child, then check */
    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));
    check_val("child exit 0", WEXITSTATUS(wstatus), 0);

    /* Bug: parent sees 42 instead of 99 — pages not shared */
    check_val("shared page visible", (long)*val, 99);

    sc2(SYS_MUNMAP, page, 4096);
}

TEST("mm/mmap-shared", test_mmap_shared);
