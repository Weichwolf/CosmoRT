#include "ktest.h"

static void test_clone_flags(void) {
    puts("\n[clone flags]\n");

    /* Allocate child stack */
    long stk = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap clone stack", stk > 0);
    if (stk <= 0) return;

    /* clone with CLONE_VM + all thread flags should succeed */
    unsigned long fl = CLONE_VM | CLONE_FS | CLONE_FILES |
                       CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM;
    long tid = sc5(SYS_CLONE, (long)fl, stk + 4096, 0, 0, 0);
    if (tid == 0) {
        /* child: exit immediately */
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }
    check("clone(VM|FS|FILES|SIGHAND|SYSVSEM) tid > 0", tid > 0);

    /* clone without CLONE_VM = fork (new process, COW).
     * musl libc's fork() uses clone(SIGCHLD, 0). */
    long r = sc5(SYS_CLONE, 17 /* SIGCHLD */, 0, 0, 0, 0);
    if (r == 0) {
        /* child: exit immediately */
        sc1(SYS_EXIT_GROUP, 42);
        __builtin_unreachable();
    }
    check("clone(SIGCHLD) = fork, pid > 0", r > 0);
    /* Reap the child */
    int wstatus = 0;
    long w = sc4(SYS_WAIT4, r, (long)&wstatus, 0, 0);
    check_val("wait4 reaps forked child", w, r);
    check_val("child exit status 42", (wstatus >> 8) & 0xff, 42);

    sc2(SYS_MUNMAP, stk, 4096);
}

TEST("clone_flags", test_clone_flags);
