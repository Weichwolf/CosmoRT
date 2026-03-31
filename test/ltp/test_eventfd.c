/* LTP eventfd01-05, eventfd2_01-03 — ported to ktest */
#include "ktest.h"

#define EFD_SEMAPHORE   1
#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

/* ── eventfd01: read errors ── */

static void test_eventfd01(void) {
    puts("\n[ltp/eventfd01]\n");

    long fd = sc2(SYS_EVENTFD2, 10, EFD_NONBLOCK);
    check("eventfd2(10, EFD_NONBLOCK)", fd >= 0);
    if (fd < 0) return;

    /* Read counter — should be 10 */
    uint64_t val = 0;
    long r = sc3(SYS_READ, fd, (long)&val, 8);
    check_val("read 8 bytes", r, 8);
    check_val("counter is 10", (long)val, 10);

    /* Counter is now 0 — read again should EAGAIN */
    r = sc3(SYS_READ, fd, (long)&val, 8);
    check_val("read zero counter EAGAIN", r, -EAGAIN);

    /* Read with buffer too small — EINVAL */
    uint32_t small;
    r = sc3(SYS_READ, fd, (long)&small, 4);
    check_val("read 4 bytes EINVAL", r, -EINVAL);

    sc1(SYS_CLOSE, fd);
}

/* ── eventfd02: write errors ── */

static void test_eventfd02(void) {
    puts("\n[ltp/eventfd02]\n");

    long fd = sc2(SYS_EVENTFD2, 0, EFD_NONBLOCK);
    check("eventfd2(0, EFD_NONBLOCK)", fd >= 0);
    if (fd < 0) return;

    /* Write and read back */
    uint64_t val = 12;
    long r = sc3(SYS_WRITE, fd, (long)&val, 8);
    check_val("write 12", r, 8);

    uint64_t buf = 0;
    sc3(SYS_READ, fd, (long)&buf, 8);
    check_val("read back 12", (long)buf, 12);

    /* Write near-max, then write again — EAGAIN */
    val = 0xFFFFFFFFFFFFFFFEULL;
    r = sc3(SYS_WRITE, fd, (long)&val, 8);
    check_val("write near-max", r, 8);

    r = sc3(SYS_WRITE, fd, (long)&val, 8);
    check_val("write overflow EAGAIN", r, -EAGAIN);

    /* Write with 4-byte buffer — EINVAL */
    uint32_t small = 1;
    r = sc3(SYS_WRITE, fd, (long)&small, 4);
    check_val("write 4 bytes EINVAL", r, -EINVAL);

    /* Write UINT64_MAX — EINVAL */
    val = 0xFFFFFFFFFFFFFFFFULL;
    r = sc3(SYS_WRITE, fd, (long)&val, 8);
    check_val("write UINT64_MAX EINVAL", r, -EINVAL);

    sc1(SYS_CLOSE, fd);
}

/* ── eventfd05: child writes, parent reads ── */

static void test_eventfd05(void) {
    puts("\n[ltp/eventfd05]\n");

    long fd = sc2(SYS_EVENTFD2, 0, EFD_NONBLOCK);
    check("eventfd2", fd >= 0);
    if (fd < 0) return;

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) { sc1(SYS_CLOSE, fd); return; }

    if (pid == 0) {
        uint64_t val = 0xDEADBEEF;
        sc3(SYS_WRITE, fd, (long)&val, 8);
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));

    uint64_t val = 0;
    long r = sc3(SYS_READ, fd, (long)&val, 8);
    check_val("read 8 bytes", r, 8);
    check_val("value from child", (long)val, (long)0xDEADBEEF);

    sc1(SYS_CLOSE, fd);
}

/* ── eventfd2_01: EFD_CLOEXEC flag ── */

static void test_eventfd2_01(void) {
    puts("\n[ltp/eventfd2_01]\n");

    /* Without EFD_CLOEXEC */
    long fd = sc2(SYS_EVENTFD2, 1, 0);
    check("eventfd2(1, 0)", fd >= 0);
    if (fd >= 0) {
        long flags = sc2(SYS_FCNTL, fd, F_GETFD);
        check("no FD_CLOEXEC", (flags & FD_CLOEXEC) == 0);
        sc1(SYS_CLOSE, fd);
    }

    /* With EFD_CLOEXEC */
    fd = sc2(SYS_EVENTFD2, 1, EFD_CLOEXEC);
    check("eventfd2(1, EFD_CLOEXEC)", fd >= 0);
    if (fd >= 0) {
        long flags = sc2(SYS_FCNTL, fd, F_GETFD);
        check("FD_CLOEXEC set", (flags & FD_CLOEXEC) != 0);
        sc1(SYS_CLOSE, fd);
    }
}

/* ── eventfd2_02: EFD_NONBLOCK flag ── */

static void test_eventfd2_02(void) {
    puts("\n[ltp/eventfd2_02]\n");

    /* Without EFD_NONBLOCK */
    long fd = sc2(SYS_EVENTFD2, 1, 0);
    check("eventfd2(1, 0)", fd >= 0);
    if (fd >= 0) {
        long flags = sc2(SYS_FCNTL, fd, F_GETFL);
        check("no O_NONBLOCK", (flags & O_NONBLOCK) == 0);
        sc1(SYS_CLOSE, fd);
    }

    /* With EFD_NONBLOCK */
    fd = sc2(SYS_EVENTFD2, 1, EFD_NONBLOCK);
    check("eventfd2(1, EFD_NONBLOCK)", fd >= 0);
    if (fd >= 0) {
        long flags = sc2(SYS_FCNTL, fd, F_GETFL);
        check("O_NONBLOCK set", (flags & O_NONBLOCK) != 0);
        sc1(SYS_CLOSE, fd);
    }
}

/* ── eventfd2_03: EFD_SEMAPHORE — read returns 1, counter decrements ── */

static void test_eventfd2_03(void) {
    puts("\n[ltp/eventfd2_03]\n");

    long fd = sc2(SYS_EVENTFD2, 0, EFD_SEMAPHORE | EFD_NONBLOCK);
    check("eventfd2(0, EFD_SEMAPHORE|EFD_NONBLOCK)", fd >= 0);
    if (fd < 0) return;

    /* Post 3 */
    uint64_t val = 3;
    long r = sc3(SYS_WRITE, fd, (long)&val, 8);
    check_val("write 3", r, 8);

    /* Semaphore read: should return 1 each time */
    uint64_t got = 0;
    r = sc3(SYS_READ, fd, (long)&got, 8);
    check_val("sem read 1", r, 8);
    check_val("sem value 1", (long)got, 1);

    r = sc3(SYS_READ, fd, (long)&got, 8);
    check_val("sem read 2", r, 8);
    check_val("sem value 1 again", (long)got, 1);

    r = sc3(SYS_READ, fd, (long)&got, 8);
    check_val("sem read 3", r, 8);
    check_val("sem value 1 third", (long)got, 1);

    /* Counter should be 0 now — EAGAIN */
    r = sc3(SYS_READ, fd, (long)&got, 8);
    check_val("sem exhausted EAGAIN", r, -EAGAIN);

    sc1(SYS_CLOSE, fd);
}

TEST("ltp/eventfd01",     test_eventfd01);
TEST("ltp/eventfd02",     test_eventfd02);
TEST("ltp/eventfd05",     test_eventfd05);
TEST("ltp/eventfd2_01",   test_eventfd2_01);
TEST("ltp/eventfd2_02",   test_eventfd2_02);
TEST("ltp/eventfd2_03",   test_eventfd2_03);
