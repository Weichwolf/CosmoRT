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

    /* timens_offsets: open RW and write CLOCK_MONOTONIC offset. */
    r = sc1(SYS_UNSHARE, CLONE_NEWTIME);
    check_val("unshare pre-offsets -> 0", r, 0);
    long tfd = sc4(SYS_OPENAT, -100, (long)"/proc/self/timens_offsets",
                   1 /*O_WRONLY*/, 0);
    check_ge("open timens_offsets", tfd, 0);
    if (tfd >= 0) {
        /* CLOCK_MONOTONIC == 1 */
        const char *line = "1 5 0\n";
        long w = sc3(SYS_WRITE, tfd, (long)line, 6);
        check("write 1 offset line", w == 6);
        /* Write bad clockid -> EINVAL */
        const char *bad = "99 5 0\n";
        w = sc3(SYS_WRITE, tfd, (long)bad, 7);
        check("write bogus clockid -> negative", w < 0);
        sc1(SYS_CLOSE, tfd);
    }
    /* Read of timens_offsets returns EACCES since we didn't install a
     * read handler; opening as O_RDONLY is allowed but read returns 0. */
    long rfd = sc4(SYS_OPENAT, -100, (long)"/proc/self/timens_offsets",
                   0 /*O_RDONLY*/, 0);
    check_ge("open timens_offsets RDONLY", rfd, 0);
    if (rfd >= 0) sc1(SYS_CLOSE, rfd);

    /* Offset application: write +123s CLOCK_MONOTONIC into
     * time_ns_for_children, fork a child, child reads clock_gettime and
     * returns (ns/1e6) mod 256 as exit code — offset ≥ 123s, so mod ≠ 0
     * only if clock keeps advancing. We simply verify the child reads a
     * much larger value than the parent (≥ 100s delta). */
    r = sc1(SYS_UNSHARE, CLONE_NEWTIME);
    check_val("unshare for offset test -> 0", r, 0);
    long ofd = sc4(SYS_OPENAT, -100, (long)"/proc/self/timens_offsets", 1, 0);
    if (ofd >= 0) {
        const char *off = "1 123 0\n";
        sc3(SYS_WRITE, ofd, (long)off, 8);
        sc1(SYS_CLOSE, ofd);
    }
    struct { long sec; long nsec; } mono_parent;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&mono_parent);
    int pipefd[2] = {0, 0};
    sc1(SYS_PIPE, (long)pipefd);
    long cpid = sc0(SYS_FORK);
    if (cpid == 0) {
        struct { long sec; long nsec; } mono_child;
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&mono_child);
        sc3(SYS_WRITE, pipefd[1], (long)&mono_child, sizeof(mono_child));
        sc1(SYS_EXIT_GROUP, 0);
    }
    sc1(SYS_CLOSE, pipefd[1]);
    struct { long sec; long nsec; } mono_child = { -1, 0 };
    sc3(SYS_READ, pipefd[0], (long)&mono_child, sizeof(mono_child));
    sc1(SYS_CLOSE, pipefd[0]);
    sc4(SYS_WAIT4, cpid, 0, 0, 0);
    check("child clock_gettime sees offset",
          mono_child.sec - mono_parent.sec >= 120);

    /* clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) sees the offset too:
     * child sleeps until (offset_based_now + 50ms). Sleep must terminate
     * in < 500ms (if it sleeps for 123s, something is wrong). */
    r = sc1(SYS_UNSHARE, CLONE_NEWTIME);
    check_val("unshare for abs-sleep -> 0", r, 0);
    long afd = sc4(SYS_OPENAT, -100, (long)"/proc/self/timens_offsets", 1, 0);
    if (afd >= 0) {
        const char *off = "1 50 0\n";
        sc3(SYS_WRITE, afd, (long)off, 7);
        sc1(SYS_CLOSE, afd);
    }
    int pfd2[2] = {0, 0};
    sc1(SYS_PIPE, (long)pfd2);
    long cpid2 = sc0(SYS_FORK);
    if (cpid2 == 0) {
        struct { long sec; long nsec; } ns_now;
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&ns_now);
        /* Deadline = now + 50ms expressed in child's virtual time */
        struct { long sec; long nsec; } deadline = {
            ns_now.sec, ns_now.nsec + 50*1000*1000
        };
        if (deadline.nsec >= 1000000000) {
            deadline.sec  += 1;
            deadline.nsec -= 1000000000;
        }
        long rc = sc4(SYS_CLOCK_NANOSLEEP, CLOCK_MONOTONIC, 1 /*ABSTIME*/,
                      (long)&deadline, 0);
        sc3(SYS_WRITE, pfd2[1], (long)&rc, sizeof(rc));
        sc1(SYS_EXIT_GROUP, 0);
    }
    sc1(SYS_CLOSE, pfd2[1]);
    long sleep_rc = -1;
    sc3(SYS_READ, pfd2[0], (long)&sleep_rc, sizeof(sleep_rc));
    sc1(SYS_CLOSE, pfd2[0]);
    sc4(SYS_WAIT4, cpid2, 0, 0, 0);
    check_val("clock_nanosleep ABS with ns-offset returned 0",
              sleep_rc, 0);
}

TEST("time_ns", test_time_ns);
/* CLONE_NEWNET acceptance moved to test/unit/net/test_net_ns.c (Phase 15). */
