#include "ktest.h"

/* CLONE_NEWTIME tests: unshare / setns / open ns handles / inheritance.
 * Full offset-application tests come once clock_gettime/nanosleep
 * consult time_ns (later commits). */

#define CLONE_NEWTIME   0x00000080

static void test_time_ns(void) {
    puts("\n[time_ns]\n");

    /* CLONE_NEWTIME on clone() is forbidden by Linux (-EINVAL). */
    long r = sc5(SYS_CLONE, CLONE_NEWTIME | 0, 0, 0, 0, 0);
    check_val("clone(CLONE_NEWTIME) -> EINVAL", r, -EINVAL);

    /* unshare(CLONE_NEWTIME) returns 0, no visible side effect on the
     * current task (Linux: only time_ns_for_children changes). */
    r = sc1(SYS_UNSHARE, CLONE_NEWTIME);
    check_val("unshare(CLONE_NEWTIME) -> 0", r, 0);

    /* unshare with bogus flag bits -> EINVAL */
    r = sc1(SYS_UNSHARE, 0xDEAD0000U);
    check_val("unshare(bogus) -> EINVAL", r, -EINVAL);

    /* /proc/self/ns/time opens as FD_NSFS. */
    long fd = sc4(SYS_OPENAT, -100, (long)"/proc/self/ns/time", 0 /*O_RDONLY*/, 0);
    check_ge("open /proc/self/ns/time", fd, 0);

    /* setns(fd, 0) accepts any ns kind. */
    r = sc2(SYS_SETNS, fd, 0);
    check_val("setns(time, 0) -> 0", r, 0);

    /* setns with wrong nstype -> EINVAL */
    r = sc2(SYS_SETNS, fd, 0x00020000 /*CLONE_NEWNS*/);
    check_val("setns(time, CLONE_NEWNS) -> EINVAL", r, -EINVAL);

    long fd2 = sc4(SYS_OPENAT, -100, (long)"/proc/self/ns/time_for_children", 0, 0);
    check_ge("open /proc/self/ns/time_for_children", fd2, 0);
    r = sc2(SYS_SETNS, fd2, CLONE_NEWTIME);
    check_val("setns(time_for_children, CLONE_NEWTIME) -> 0", r, 0);

    /* Close everything. */
    sc1(SYS_CLOSE, fd);
    sc1(SYS_CLOSE, fd2);

    /* After unshare, fork() child should observe the new NS
     * (refcount propagates; we can only assert the fork itself works). */
    r = sc1(SYS_UNSHARE, CLONE_NEWTIME);
    check_val("unshare(CLONE_NEWTIME) #2 -> 0", r, 0);
    long pid = sc0(SYS_FORK);
    if (pid == 0) sc1(SYS_EXIT_GROUP, 42);
    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("fork after unshare exited 42", (status >> 8) == 42);
}

TEST("time_ns", test_time_ns);
