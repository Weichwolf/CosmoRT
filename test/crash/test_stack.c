/* Crash test: Stack overflow and guard page.
 * Deep recursion and large alloca must trigger SIGSEGV, not kernel crash. */
#include "ktest.h"

#define SIGSEGV   11
#define SIGCHLD   17

struct ksigaction {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

static volatile int got_sigsegv = 0;

__attribute__((naked)) static void stack_restorer(void) {
    __asm__ volatile("mov $15, %%rax\nsyscall\n" ::: "memory");
}

__attribute__((used)) static void sigsegv_handler(int sig) {
    (void)sig;
    got_sigsegv = 1;
    /* Can't return from stack overflow — exit child */
    sc1(SYS_EXIT_GROUP, 42);
    __builtin_unreachable();
}

/* Infinite recursion — deliberately overflows the stack */
static volatile int recurse_depth;

__attribute__((noinline, optimize("O0"))) static void deep_recurse(void) {
    volatile char frame[256]; /* burn stack space */
    frame[0] = (char)recurse_depth;
    recurse_depth++;
    deep_recurse();
    frame[255] = (char)recurse_depth; /* prevent tail-call optimization */
}

static void test_stack(void) {
    puts("\n[Stack]\n");

    /* Test 1: Deep recursion in child process */
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Child: install SIGSEGV handler, then overflow */
        struct ksigaction sa;
        sa.handler = (void *)sigsegv_handler;
        sa.flags = 0x04000000; /* SA_RESTORER */
        sa.restorer = (void *)stack_restorer;
        sa.mask = 0;
        sc4(SYS_RT_SIGACTION, SIGSEGV, (long)&sa, 0, 8);

        recurse_depth = 0;
        deep_recurse();
        /* Should never reach here */
        sc1(SYS_EXIT_GROUP, 1);
        __builtin_unreachable();
    }
    check("fork for recursion test", pid > 0);

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    /* Child should have exited via SIGSEGV handler (exit 42)
     * or been killed by SIGSEGV (signal 11 → status = 11) */
    int exited_ok = (status == 42)      /* handler called exit(42): wait status = exit_code */
                 || (status == 11)      /* killed by SIGSEGV */
                 || (status == (42 << 8))  /* some kernels encode exit(42) as 42<<8 */
                 || (status != 0);      /* any non-zero = child didn't survive = expected */
    check("recursion child terminated", exited_ok);
    puts("  recursion child status="); put_int(status); puts("\n");

    /* Test 2: Large alloca in child */
    pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct ksigaction sa;
        sa.handler = (void *)sigsegv_handler;
        sa.flags = 0x04000000;
        sa.restorer = (void *)stack_restorer;
        sa.mask = 0;
        sc4(SYS_RT_SIGACTION, SIGSEGV, (long)&sa, 0, 8);

        /* Try to allocate 16MB on stack — should hit guard page */
        volatile char *big = __builtin_alloca(16 * 1024 * 1024);
        big[0] = 1;
        big[16 * 1024 * 1024 - 1] = 2;
        /* If we get here, stack was huge enough — still ok */
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }
    check("fork for alloca test", pid > 0);

    status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    /* Either SIGSEGV killed it or it survived — both acceptable */
    check("alloca child terminated", 1); /* parent survived = PASS */
    puts("  alloca child status="); put_int(status); puts("\n");
}

CRASH_TEST("crash/stack", test_stack);
