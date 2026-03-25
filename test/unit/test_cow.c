#include "ktest.h"

/* COW fork: basic — fork and child exits immediately */
static void test_cow_basic(void) {
    puts("\n[COW basic]\n");

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);

    if (pid == 0) {
        sc1(SYS_EXIT_GROUP, 0);
    } else {
        int status = 0;
        sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
        check("child exited 0", (status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0);
    }
}

/* COW fork: child write triggers private copy, parent unchanged */
static void test_cow_fork_write(void) {
    puts("\n[COW fork write]\n");

    long addr = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap", addr > 0x1000);
    volatile uint64_t *p = (volatile uint64_t *)addr;
    *p = 0xDEADBEEF;

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);

    if (pid == 0) {
        /* Child: verify parent's value, then write own */
        if (*p != 0xDEADBEEF)
            sc1(SYS_EXIT_GROUP, 1);
        *p = 0xCAFEBABE;
        if (*p != 0xCAFEBABE)
            sc1(SYS_EXIT_GROUP, 2);
        sc1(SYS_EXIT_GROUP, 0);
    } else {
        int status = 0;
        sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
        check("child exited 0", (status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0);
        check_val("parent unchanged", (long)*p, (long)0xDEADBEEF);
        sc2(SYS_MUNMAP, addr, 4096);
    }
}

/* COW fork: no write → no page copies */
static void test_cow_no_write(void) {
    puts("\n[COW no-write]\n");

    long addr = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap", addr > 0x1000);
    *(volatile uint64_t *)addr = 0x42;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Child: read-only access */
        sc1(SYS_EXIT_GROUP, *(volatile uint64_t *)addr != 0x42);
    }
    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child read-only ok", (status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0);
    sc2(SYS_MUNMAP, addr, 4096);
}

/* COW fork: child exit decrements refcount, parent can still write */
static void test_cow_exit_refcount(void) {
    puts("\n[COW exit refcount]\n");

    long addr = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap", addr > 0x1000);
    *(volatile uint64_t *)addr = 0x1234;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_EXIT_GROUP, 0);
    }
    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child exited", (status & 0x7f) == 0);
    /* Parent writes: refcount should be 1 now, COW resolves in-place */
    *(volatile uint64_t *)addr = 0x5678;
    check_val("parent write ok", (long)*(volatile uint64_t *)addr, 0x5678);
    sc2(SYS_MUNMAP, addr, 4096);
}

/* COW fork: sequential forks share pages correctly */
static void test_cow_multi_fork(void) {
    puts("\n[COW multi-fork]\n");

    long addr = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap", addr > 0x1000);
    *(volatile uint64_t *)addr = 0xAAAA;

    /* Fork child 1 and wait */
    long pid1 = sc0(SYS_FORK);
    if (pid1 == 0) {
        *(volatile uint64_t *)addr = 0xBBBB;
        sc1(SYS_EXIT_GROUP, *(volatile uint64_t *)addr != 0xBBBB);
    }
    int s1 = 0;
    sc4(SYS_WAIT4, pid1, (long)&s1, 0, 0);
    check("child1 ok", (s1 & 0x7f) == 0 && ((s1 >> 8) & 0xff) == 0);
    check_val("parent after child1", (long)*(volatile uint64_t *)addr, (long)0xAAAA);

    /* Fork child 2 and wait */
    long pid2 = sc0(SYS_FORK);
    if (pid2 == 0) {
        *(volatile uint64_t *)addr = 0xCCCC;
        sc1(SYS_EXIT_GROUP, *(volatile uint64_t *)addr != 0xCCCC);
    }
    int s2 = 0;
    sc4(SYS_WAIT4, pid2, (long)&s2, 0, 0);
    check("child2 ok", (s2 & 0x7f) == 0 && ((s2 >> 8) & 0xff) == 0);
    check_val("parent still 0xAAAA", (long)*(volatile uint64_t *)addr, (long)0xAAAA);
    sc2(SYS_MUNMAP, addr, 4096);
}

TEST("cow_basic", test_cow_basic);
TEST("cow_fork_write", test_cow_fork_write);
TEST("cow_no_write", test_cow_no_write);
TEST("cow_exit_refcount", test_cow_exit_refcount);
TEST("cow_multi_fork", test_cow_multi_fork);
