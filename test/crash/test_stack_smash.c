/* Test: find which syscall corrupts stack canary. */
#include "ktest.h"

#define CANARY 0xDEADBEEFCAFEBABEULL

#define GUARD_TEST(desc, code) do { \
    volatile uint64_t _lo = CANARY; \
    char _buf[512]; \
    volatile uint64_t _hi = CANARY; \
    (void)_buf; \
    { code; } \
    if (_lo != CANARY || _hi != CANARY) \
        fail(desc, "canary corrupted"); \
    else \
        pass(desc); \
} while(0)

static void test_stack_smash(void) {
    puts("\n[Stack Smash Probe]\n");

    GUARD_TEST("getcwd", { sc2(SYS_getcwd, (long)_buf, 256); });
    GUARD_TEST("uname", { sc1(SYS_uname, (long)_buf); });
    GUARD_TEST("sysinfo", { sc1(99, (long)_buf); });
    GUARD_TEST("getrandom", { sc3(318, (long)_buf, 64, 0); });
    GUARD_TEST("ioctl TCGETS", { sc3(16, 0, 0x5401, (long)_buf); });
    GUARD_TEST("ioctl TIOCGWINSZ", { sc3(16, 0, 0x5413, (long)_buf); });
    GUARD_TEST("prlimit64", { sc4(302, 0, 7, 0, (long)_buf); });
    GUARD_TEST("sched_getaffinity", { sc3(204, 0, 128, (long)_buf); });
    GUARD_TEST("fstat", { sc2(SYS_fstat, 0, (long)_buf); });
    GUARD_TEST("readlink", { sc3(89, (long)"/proc/self/exe", (long)_buf, 256); });
    GUARD_TEST("clock_gettime", { sc2(228, 1, (long)_buf); });
    GUARD_TEST("getrusage", { sc2(98, 0, (long)_buf); });
    GUARD_TEST("times", { sc1(100, (long)_buf); });
    GUARD_TEST("sigaction old", { sc4(SYS_rt_sigaction, 10, 0, (long)_buf, 8); });
    GUARD_TEST("getdents64", {
        long fd = sc3(SYS_open, (long)"/proc", 0, 0);
        if (fd >= 0) { sc3(217, fd, (long)_buf, 256); sc1(SYS_close, fd); }
    });
}

CRASH_TEST("crash/stack_smash", test_stack_smash);
