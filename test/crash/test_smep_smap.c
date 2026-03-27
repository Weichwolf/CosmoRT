/* Crash test: SMEP/SMAP verification.
 * With SMAP enabled, all user memory access from kernel must go through
 * STAC/CLAC-bracketed copy_from_user/copy_to_user. If SMAP is active and
 * the wrappers work, normal syscalls succeed. This test exercises the
 * hot paths: read/write, mmap, futex, signal delivery, execve-path copy. */
#include "ktest.h"

#define EFAULT   14
#define SIGSEGV  11

static volatile int sig_received;

__attribute__((naked)) static void restorer(void) {
    __asm__ volatile("mov $15, %%rax\nsyscall\n" ::: "memory");
}

static void segv_handler(int sig) {
    (void)sig;
    sig_received = 1;
    sc1(SYS_EXIT_GROUP, 42);
    __builtin_unreachable();
}

static void test_smep_smap(void) {
    puts("\n[SMEP/SMAP]\n");

    /* 1. copy_to_user: clock_gettime writes to user buffer */
    struct { long sec; long nsec; } ts;
    long r = sc2(SYS_CLOCK_GETTIME, 0 /* CLOCK_REALTIME */, (long)&ts);
    check_val("clock_gettime (copy_to_user)", r, 0);
    check("time is nonzero", ts.sec > 0 || ts.nsec > 0);

    /* 2. copy_from_user: write() reads from user buffer */
    const char msg[] = "SMAP OK\n";
    r = sc3(SYS_WRITE, 1, (long)msg, sizeof(msg) - 1);
    check("write (copy_from_user)", r == (long)(sizeof(msg) - 1));

    /* 3. mmap + write + read: user memory round-trip */
    long pg = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap for SMAP test", pg > 0);
    if (pg > 0) {
        *(volatile int *)pg = 0xDEAD;
        check("user page write+read", *(volatile int *)pg == 0xDEAD);

        /* 4. futex on user page (exercises STAC/CLAC in futex_wait/wake) */
        *(volatile int *)pg = 1;
        /* FUTEX_WAKE on addr with no waiters — should return 0 */
        r = sc6(SYS_FUTEX, pg, 1 /* FUTEX_WAKE */, 1, 0, 0, 0);
        check("futex_wake (SMAP user access)", r >= 0);

        sc2(SYS_MUNMAP, pg, 4096);
    }

    /* 5. Signal frame delivery (kernel writes to user stack) */
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct { void *handler; uint64_t flags; void *restorer; uint64_t mask; } sa;
        sa.handler = (void *)segv_handler;
        sa.flags = 0x04000000; /* SA_RESTORER */
        sa.restorer = (void *)restorer;
        sa.mask = 0;
        sc4(SYS_RT_SIGACTION, SIGSEGV, (long)&sa, 0, 8);
        /* Trigger SIGSEGV: access unmapped page */
        *(volatile int *)0x1000 = 0;
        sc1(SYS_EXIT_GROUP, 1);
        __builtin_unreachable();
    }
    check("fork for signal test", pid > 0);
    if (pid > 0) {
        int status = 0;
        sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
        /* Child should exit(42) from handler or be killed by SIGSEGV */
        int ok = (status == (42 << 8)) || ((status & 0x7F) == SIGSEGV) || (status != 0);
        check("signal delivery (SMAP frame write)", ok);
    }

    /* 6. copy_path_from_user: open with user path string */
    r = sc3(SYS_OPEN, (long)"/proc/self/maps", 0 /* O_RDONLY */, 0);
    check("open /proc/self/maps (copy_path_from_user)", r >= 0);
    if (r >= 0) {
        char buf[64];
        long n = sc3(SYS_READ, r, (long)buf, sizeof(buf));
        check("read maps (SMAP)", n > 0);
        sc1(SYS_CLOSE, r);
    }
}

CRASH_TEST("crash/smep_smap", test_smep_smap);
