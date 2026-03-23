/* Crash test: Deep recursion until stack overflow.
 * Kernel must deliver SIGSEGV to the child, not panic. */
#include "ktest.h"

#define SIGSEGV      11
#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)
#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

static volatile int depth;

/* Burn 512 bytes per frame to overflow faster */
__attribute__((noinline, optimize("O0")))
static void recurse_forever(void) {
    volatile char frame[512];
    frame[0] = (char)(++depth);
    frame[511] = (char)depth;
    recurse_forever();
    /* Prevent tail-call elimination */
    __asm__ volatile("" :: "r"(frame[0]), "r"(frame[511]));
}

static void test_deep_recursion(void) {
    puts("\n[Deep Recursion]\n");

    /* Test 1: recursion without signal handler — kernel delivers SIGSEGV, kills child */
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        depth = 0;
        recurse_forever();
        sc1(SYS_EXIT_GROUP, 1); /* unreachable */
        __builtin_unreachable();
    }
    check("fork for recursion (no handler)", pid > 0);

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    /* Accept WIFSIGNALED(SIGSEGV) or WIFEXITED(128+SIGSEGV=139) — both mean
     * the kernel correctly handled the stack overflow. CosmoRT may encode
     * signal death as exit(128+sig). */
    int sig_death = WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
    int exit_139  = WIFEXITED(status) && WEXITSTATUS(status) == (128 + SIGSEGV);
    check("child killed by SIGSEGV", sig_death || exit_139);
    puts("  status=0x"); put_hex((uint64_t)status); puts("\n");

    /* Test 2: second child — same test, confirm no kernel state leak */
    pid = sc0(SYS_FORK);
    if (pid == 0) {
        depth = 0;
        recurse_forever();
        sc1(SYS_EXIT_GROUP, 1);
        __builtin_unreachable();
    }
    check("fork for recursion (2nd)", pid > 0);

    status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    sig_death = WIFSIGNALED(status);
    exit_139  = WIFEXITED(status) && WEXITSTATUS(status) >= 128;
    check("2nd child also killed", sig_death || exit_139);
    puts("  status="); put_int(status); puts("\n");

    /* Test 3: parent survives — can still allocate and fork */
    long pg = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("parent mmap after recursion crash", pg > 0);
    if (pg > 0) {
        *(volatile char *)pg = 0x42;
        sc2(SYS_MUNMAP, pg, 4096);
    }

    pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }
    check("parent can fork after crash", pid > 0);
    if (pid > 0) {
        status = 0;
        sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
        check("post-crash child exited 0", WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}

CRASH_TEST("crash/deep_recursion", test_deep_recursion);
