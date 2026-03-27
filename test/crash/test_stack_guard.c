/* Crash test: Stack guard page.
 * Verify that deep recursion hits a guard page and gets SIGSEGV,
 * not silent corruption. Guard pages are PROT_NONE VMAs below the stack. */
#include "ktest.h"

#define SIGSEGV      11
#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)
#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

/* Burn stack fast: 1KB per frame */
__attribute__((noinline, optimize("O0")))
static void overflow_stack(void) {
    volatile char frame[1024];
    frame[0] = 1; frame[1023] = 2;
    overflow_stack();
    __asm__ volatile("" :: "r"(frame[0]), "r"(frame[1023]));
}

static void test_stack_guard(void) {
    puts("\n[Stack Guard Page]\n");

    /* Test 1: child overflows stack → SIGSEGV from guard page */
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        overflow_stack();
        sc1(SYS_EXIT_GROUP, 1); /* unreachable */
        __builtin_unreachable();
    }
    check("fork for stack guard test", pid > 0);

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    int killed_by_sig = WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
    int exit_139 = WIFEXITED(status) && WEXITSTATUS(status) == (128 + SIGSEGV);
    check("stack overflow → SIGSEGV", killed_by_sig || exit_139);
    puts("  status=0x"); put_hex((uint64_t)status); puts("\n");

    /* Test 2: parent still healthy after child crash */
    long pg = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("parent mmap after child stack overflow", pg > 0);
    if (pg > 0) {
        *(volatile char *)pg = 0x55;
        check("parent memory intact", *(volatile char *)pg == 0x55);
        sc2(SYS_MUNMAP, pg, 4096);
    }

    /* Test 3: verify guard page shows in /proc/self/maps as PROT_NONE VMA.
     * Read maps, look for "---p" (no permissions = guard). */
    long fd = sc3(SYS_OPEN, (long)"/proc/self/maps", 0, 0);
    if (fd >= 0) {
        char buf[2048];
        long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            /* Search for "---p" which indicates a PROT_NONE mapping */
            int found = 0;
            for (long i = 0; i < n - 4; i++) {
                if (buf[i] == '-' && buf[i+1] == '-' && buf[i+2] == '-' && buf[i+3] == 'p') {
                    found = 1;
                    break;
                }
            }
            check("guard page VMA in /proc/self/maps", found);
        }
        sc1(SYS_CLOSE, fd);
    }
}

CRASH_TEST("crash/stack_guard", test_stack_guard);
