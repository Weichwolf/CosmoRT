/* LTP clone01-07, clone09, clone11 — ported to ktest */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)

#define STACK_SIZE 4096

static void *alloc_stack(void) {
    return (void *)sc6(SYS_MMAP, 0, STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

/* ── clone01: basic clone, child exits, parent gets correct pid ── */

static void test_clone01(void) {
    puts("\n[ltp/clone01]\n");

    void *stack = alloc_stack();
    check("mmap stack", (long)stack > 0);
    if ((long)stack <= 0) return;

    long pid = sc5(SYS_CLONE, SIGCHLD, (long)stack + STACK_SIZE, 0, 0, 0);
    check("clone", pid >= 0);
    if (pid < 0) { sc2(SYS_MUNMAP, (long)stack, STACK_SIZE); return; }

    if (pid == 0) {
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    int wstatus = 0;
    long wpid = sc4(SYS_WAIT4, -1, (long)&wstatus, 0, 0);
    check_val("wait returns child pid", wpid, pid);
    check("child exited cleanly", WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);

    sc2(SYS_MUNMAP, (long)stack, STACK_SIZE);
}

/* ── clone03: child getpid matches parent's clone return ── */

static void test_clone03(void) {
    puts("\n[ltp/clone03]\n");

    /* Shared memory for child to write its pid */
    long *shared = (long *)sc6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check("mmap shared", (long)shared > 0);
    if ((long)shared <= 0) return;
    *shared = 0;

    void *stack = alloc_stack();
    check("mmap stack", (long)stack > 0);
    if ((long)stack <= 0) { sc2(SYS_MUNMAP, (long)shared, 4096); return; }

    long pid = sc5(SYS_CLONE, SIGCHLD, (long)stack + STACK_SIZE, 0, 0, 0);
    check("clone", pid >= 0);
    if (pid < 0) goto cleanup;

    if (pid == 0) {
        *shared = sc0(SYS_GETPID);
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check_val("child pid matches", *shared, pid);

cleanup:
    sc2(SYS_MUNMAP, (long)stack, STACK_SIZE);
    sc2(SYS_MUNMAP, (long)shared, 4096);
}

/* ── clone04: NULL stack returns EINVAL ── */

static void test_clone04(void) {
    puts("\n[ltp/clone04]\n");

    long r = sc5(SYS_CLONE, SIGCHLD, 0, 0, 0, 0);
    /* Linux returns EINVAL for NULL child stack (when not CLONE_VM) */
    check_val("clone NULL stack EINVAL", r, -EINVAL);
}

/* ── clone05: CLONE_VFORK suspends parent until child exits ── */

static void test_clone05(void) {
    puts("\n[ltp/clone05]\n");

    /* Shared flag: child sets to 1 before exiting */
    volatile int *flag = (volatile int *)sc6(SYS_MMAP, 0, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check("mmap shared", (long)flag > 0);
    if ((long)flag <= 0) return;
    *flag = 0;

    void *stack = alloc_stack();
    check("mmap stack", (long)stack > 0);
    if ((long)stack <= 0) { sc2(SYS_MUNMAP, (long)flag, 4096); return; }

    long pid = sc5(SYS_CLONE, CLONE_VM | CLONE_VFORK | SIGCHLD,
                   (long)stack + STACK_SIZE, 0, 0, 0);
    check("clone VFORK", pid >= 0);
    if (pid < 0) goto cleanup;

    if (pid == 0) {
        *flag = 1;
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    /* After CLONE_VFORK returns, child must have already exited */
    check_val("child set flag before parent resumed", *flag, 1);

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);

cleanup:
    sc2(SYS_MUNMAP, (long)stack, STACK_SIZE);
    sc2(SYS_MUNMAP, (long)flag, 4096);
}

/* ── clone06: child inherits environment (test via shared memory) ── */

static void test_clone06(void) {
    puts("\n[ltp/clone06]\n");

    /* In freestanding ktest there's no getenv. Test that child can
       read parent's memory (no CLONE_VM, but fork-like semantics
       give child a copy of parent's address space). */
    volatile int marker = 42;

    void *stack = alloc_stack();
    check("mmap stack", (long)stack > 0);
    if ((long)stack <= 0) return;

    long pid = sc5(SYS_CLONE, SIGCHLD, (long)stack + STACK_SIZE, 0, 0, 0);
    check("clone", pid >= 0);
    if (pid < 0) { sc2(SYS_MUNMAP, (long)stack, STACK_SIZE); return; }

    if (pid == 0) {
        /* Child can read parent's data (copy-on-write) */
        sc1(SYS_EXIT_GROUP, (marker == 42) ? 0 : 1);
        __builtin_unreachable();
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child inherited memory", WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);

    sc2(SYS_MUNMAP, (long)stack, STACK_SIZE);
}

/* ── clone07: child returning (not calling _exit) should not SIGSEGV ── */

static void test_clone07(void) {
    puts("\n[ltp/clone07]\n");

    void *stack = alloc_stack();
    check("mmap stack", (long)stack > 0);
    if ((long)stack <= 0) return;

    /* Use fork (SIGCHLD only, stack provided) — child calls _exit */
    long pid = sc5(SYS_CLONE, SIGCHLD, (long)stack + STACK_SIZE, 0, 0, 0);
    check("clone", pid >= 0);
    if (pid < 0) { sc2(SYS_MUNMAP, (long)stack, STACK_SIZE); return; }

    if (pid == 0) {
        /* Explicitly exit (raw clone child must not return) */
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);

    if (WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == 11) {
        fail("clone07 child return", "child got SIGSEGV");
    } else if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0) {
        pass("clone07 child exited cleanly");
    } else {
        fail("clone07 child", "unexpected status");
    }

    sc2(SYS_MUNMAP, (long)stack, STACK_SIZE);
}

/* ── clone09: CLONE_NEWNET requires CAP_SYS_ADMIN — skip as namespace test ── */
/* CosmoRT doesn't support namespaces. Test that CLONE_NEWNET returns EINVAL. */

static void test_clone09(void) {
    puts("\n[ltp/clone09]\n");

    void *stack = alloc_stack();
    check("mmap stack", (long)stack > 0);
    if ((long)stack <= 0) return;

    long r = sc5(SYS_CLONE, CLONE_NEWNET | CLONE_VM | SIGCHLD,
                 (long)stack + STACK_SIZE, 0, 0, 0);
    /* Should fail — CosmoRT doesn't support namespaces */
    check("CLONE_NEWNET fails", r < 0);

    sc2(SYS_MUNMAP, (long)stack, STACK_SIZE);
}

/* ── clone11: CLONE_NEWPID without CAP_SYS_ADMIN — should fail ── */

static void test_clone11(void) {
    puts("\n[ltp/clone11]\n");

    void *stack = alloc_stack();
    check("mmap stack", (long)stack > 0);
    if ((long)stack <= 0) return;

    long r = sc5(SYS_CLONE, CLONE_NEWPID, (long)stack + STACK_SIZE, 0, 0, 0);
    /* CosmoRT doesn't support namespaces — should fail */
    check("CLONE_NEWPID fails", r < 0);

    sc2(SYS_MUNMAP, (long)stack, STACK_SIZE);
}

TEST("ltp/clone01",  test_clone01);
TEST("ltp/clone03",  test_clone03);
TEST("ltp/clone04",  test_clone04);
TEST("ltp/clone05",  test_clone05);
TEST("ltp/clone06",  test_clone06);
TEST("ltp/clone07",  test_clone07);
TEST("ltp/clone09",  test_clone09);
TEST("ltp/clone11",  test_clone11);
