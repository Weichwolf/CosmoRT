/* LTP security tests — ported to ktest
 * kernbench: build system benchmark — skip (not a syscall test)
 * meltdown: test kernel memory isolation
 * stack_clash: test stack guard page */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)

/* ── kernbench: skip ── */

static void test_kernbench(void) {
    puts("\n[ltp/kernbench]\n");
    /* kernbench is a build system benchmark (make -j on kernel source).
     * Not a syscall test. Nothing to port. */
    pass("kernbench is a benchmark, skip");
}

/* ── meltdown: reading kernel memory should cause SIGSEGV ──
 * Fork a child that tries to read kernel-space address.
 * Parent checks child was killed by SIGSEGV. */

static void test_meltdown(void) {
    puts("\n[ltp/meltdown]\n");

    long pid = sc5(SYS_CLONE, SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        /* Child: try to read kernel memory at 0xffffffff80000000 */
        volatile char *p = (volatile char *)0xffffffff80000000UL;
        volatile char c = *p;
        (void)c;
        /* If we get here, no SIGSEGV — that's bad */
        sc1(SYS_EXIT_GROUP, 1);
    }
    check("fork", pid > 0);
    if (pid <= 0) return;

    int st;
    sc4(SYS_WAIT4, pid, (long)&st, 0, 0);

    if (WIFSIGNALED(st) && WTERMSIG(st) == 11 /* SIGSEGV */) {
        pass("child got SIGSEGV reading kernel memory");
    } else if (WIFEXITED(st) && WEXITSTATUS(st) == 1) {
        fail("child read kernel memory without SIGSEGV", "meltdown vulnerable?");
    } else {
        /* SIGBUS or other signal is also acceptable */
        check("child terminated by signal", WIFSIGNALED(st));
    }
}

/* ── stack_clash: stack guard page prevents growth into mmap ──
 * Fork a child that:
 * 1. mmaps a page near (below) the stack
 * 2. Recurses to exhaust stack
 * 3. Should get SIGSEGV from guard page, not overwrite the mmap
 *
 * Simplified: just verify that deep recursion causes SIGSEGV in child. */

/* noinline + O0 + asm-sink defeats tail-call optimisation; without it, GCC turns
 * this self-recursion into a loop and the stack never grows — the test would
 * pass trivially without actually exercising the guard page. */
__attribute__((noinline, optimize("O0")))
static void stack_recurse(volatile int depth) {
    volatile char buf[1024];
    buf[0] = (char)depth;
    buf[1023] = (char)depth;
    if (depth < 100000)
        stack_recurse(depth + 1);
    __asm__ volatile("" :: "r"(buf[0]), "r"(buf[1023]));
}

static void test_stack_clash(void) {
    puts("\n[ltp/stack_clash]\n");

    long pid = sc5(SYS_CLONE, SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        /* Child: recurse until stack overflow */
        stack_recurse(0);
        /* Should not reach here */
        sc1(SYS_EXIT_GROUP, 1);
    }
    check("fork", pid > 0);
    if (pid <= 0) return;

    int st;
    sc4(SYS_WAIT4, pid, (long)&st, 0, 0);

    if (WIFSIGNALED(st)) {
        int sig = WTERMSIG(st);
        /* SIGSEGV=11 or SIGBUS=7 are acceptable */
        check("child killed by SIGSEGV or SIGBUS", sig == 11 || sig == 7);
    } else {
        fail("child exited normally", "stack overflow not caught");
    }
}

TEST("ltp/kernbench",   test_kernbench);
TEST("ltp/meltdown",    test_meltdown);
TEST("ltp/stack_clash", test_stack_clash);
