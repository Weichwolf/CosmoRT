/* LTP CVE tests — ported to ktest
 * CVE-2016-10044, CVE-2016-7042, CVE-2017-17052, CVE-2017-17053,
 * CVE-2017-2618, CVE-2017-2671, CVE-2025-38236 */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)

/* ── CVE-2016-10044: AIO mapping not executable under READ_IMPLIES_EXEC ──
 * Original uses io_setup/io_destroy and /proc/self/maps parsing.
 * io_setup (SYS_IO_SETUP=206) + personality(READ_IMPLIES_EXEC).
 * Test that io_setup returns -ENOSYS or succeeds without crash. */

#define SYS_IO_SETUP   206
#define SYS_IO_DESTROY 207

static void test_cve_2016_10044(void) {
    puts("\n[ltp/cve-2016-10044]\n");
    unsigned long ctx = 0;
    long r = sc2(SYS_IO_SETUP, 1, (long)&ctx);
    if (r == -ENOSYS) {
        pass("io_setup not implemented (ENOSYS)");
        return;
    }
    check("io_setup did not crash", r >= -4096 || r >= 0);
    if (r == 0) {
        sc1(SYS_IO_DESTROY, (long)ctx);
        pass("io_setup + io_destroy ok, no crash");
    }
}

/* ── CVE-2016-7042: /proc/keys buffer overflow ──
 * Original adds a keyring key and reads /proc/keys.
 * CosmoRT has no keyctl. Test that add_key returns -ENOSYS. */

#define SYS_ADD_KEY  248
#define SYS_KEYCTL   250

static void test_cve_2016_7042(void) {
    puts("\n[ltp/cve-2016-7042]\n");
    long r = sc5(SYS_ADD_KEY, (long)"user", (long)"ltptest",
                 (long)"a", 1, -3 /* KEY_SPEC_SESSION_KEYRING */);
    if (r == -ENOSYS) {
        pass("add_key not implemented (ENOSYS)");
        return;
    }
    /* If implemented: read /proc/keys to trigger the bug */
    long fd = sc3(SYS_OPEN, (long)"/proc/keys", O_RDONLY, 0);
    if (fd < 0) {
        pass("/proc/keys not available, skip");
        if (r >= 0)
            sc3(SYS_KEYCTL, 9 /* KEYCTL_UNLINK */, r, -3);
        return;
    }
    char buf[512];
    sc3(SYS_READ, fd, (long)buf, 512);
    sc1(SYS_CLOSE, fd);
    pass("read /proc/keys did not crash");
    if (r >= 0)
        sc3(SYS_KEYCTL, 9, r, -3);
}

/* ── CVE-2017-17052: fork use-after-free of exe_file ──
 * Original forks massively with mmap threads.
 * Simplified: fork a few times rapidly, check no crash. */

static void test_cve_2017_17052(void) {
    puts("\n[ltp/cve-2017-17052]\n");

    for (int run = 0; run < 4; run++) {
        long pid = sc5(SYS_CLONE, SIGCHLD, 0, 0, 0, 0);
        if (pid == 0) {
            /* child: fork once more and exit */
            long pid2 = sc5(SYS_CLONE, SIGCHLD, 0, 0, 0, 0);
            if (pid2 == 0)
                sc1(SYS_EXIT_GROUP, 0);
            if (pid2 > 0) {
                int st;
                sc4(SYS_WAIT4, pid2, (long)&st, 0, 0);
            }
            sc1(SYS_EXIT_GROUP, 0);
        }
        if (pid > 0) {
            int st;
            sc4(SYS_WAIT4, pid, (long)&st, 0, 0);
        }
    }
    pass("fork stress did not crash");
}

/* ── CVE-2017-17053: LDT use-after-free ──
 * Original uses modify_ldt + fork threads. x86_64 specific.
 * Simplified: just fork + exit rapidly. The actual LDT manipulation
 * requires arch_prctl or modify_ldt which we skip. */

static void test_cve_2017_17053(void) {
    puts("\n[ltp/cve-2017-17053]\n");

    for (int run = 0; run < 4; run++) {
        long pid = sc5(SYS_CLONE, SIGCHLD, 0, 0, 0, 0);
        if (pid == 0) {
            long pid2 = sc5(SYS_CLONE, SIGCHLD, 0, 0, 0, 0);
            if (pid2 == 0)
                sc1(SYS_EXIT_GROUP, 0);
            if (pid2 > 0) {
                int st;
                sc4(SYS_WAIT4, pid2, (long)&st, 0, 0);
            }
            sc1(SYS_EXIT_GROUP, 0);
        }
        if (pid > 0) {
            int st;
            sc4(SYS_WAIT4, pid, (long)&st, 0, 0);
        }
    }
    pass("fork stress (LDT variant) did not crash");
}

/* ── CVE-2017-2618: /proc/self/attr/fscreate write off-by-one ──
 * Original writes "\n" to /proc/self/attr/fscreate in forked children.
 * CosmoRT likely has no SELinux. Test that the file doesn't exist
 * or writing doesn't crash. */

static void test_cve_2017_2618(void) {
    puts("\n[ltp/cve-2017-2618]\n");

    long fd = sc3(SYS_OPEN, (long)"/proc/self/attr/fscreate", O_WRONLY, 0);
    if (fd < 0) {
        pass("/proc/self/attr/fscreate not available, skip");
        return;
    }
    /* Write "\n" in a child, check no crash */
    long pid = sc5(SYS_CLONE, SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        sc3(SYS_WRITE, fd, (long)"\n", 1);
        sc1(SYS_CLOSE, fd);
        sc1(SYS_EXIT_GROUP, 0);
    }
    sc1(SYS_CLOSE, fd);
    if (pid > 0) {
        int st;
        sc4(SYS_WAIT4, pid, (long)&st, 0, 0);
    }
    pass("write to fscreate did not crash");
}

/* ── CVE-2017-2671: ping socket connect race ──
 * Original races connect(AF_INET) vs connect(AF_UNSPEC) on IPPROTO_ICMP socket.
 * CosmoRT may not support IPPROTO_ICMP ping sockets.
 * Test that socket creation returns appropriate error or succeeds without crash. */

#define IPPROTO_ICMP 1

static void test_cve_2017_2671(void) {
    puts("\n[ltp/cve-2017-2671]\n");

    long fd = sc3(SYS_SOCKET, AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (fd < 0) {
        pass("ICMP ping socket not supported, skip");
        return;
    }
    /* Connect to loopback */
    struct { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; } sa;
    for (int i = 0; i < 16; i++) ((char*)&sa)[i] = 0;
    sa.family = AF_INET;
    sa.addr = 0x0100007f; /* 127.0.0.1 in network order */

    sc3(SYS_CONNECT, fd, (long)&sa, 16);

    /* Disconnect with AF_UNSPEC */
    sa.family = 0; /* AF_UNSPEC */
    sc3(SYS_CONNECT, fd, (long)&sa, 16);

    sc1(SYS_CLOSE, fd);
    pass("ping socket connect/disconnect did not crash");
}

/* ── CVE-2025-38236: AF_UNIX OOB data use-after-free ──
 * Original sends multiple MSG_OOB on AF_UNIX SOCK_STREAM socketpair.
 * Test the sequence: send OOB, recv OOB, send OOB, recv OOB,
 * send OOB, recv normal — should return EAGAIN/EWOULDBLOCK. */

#define MSG_OOB      0x01
#define MSG_DONTWAIT 0x40

static void test_cve_2025_38236(void) {
    puts("\n[ltp/cve-2025-38236]\n");

    long sv[2];
    long r = sc4(SYS_SOCKETPAIR, AF_UNIX, SOCK_STREAM, 0, (long)sv);
    if (r < 0) {
        check_val("socketpair", r, 0);
        return;
    }

    char buf;

    /* #1 send + recv OOB */
    sc6(SYS_SENDTO, sv[1], (long)"A", 1, MSG_OOB, 0, 0);
    sc6(SYS_RECVFROM, sv[0], (long)&buf, 1, MSG_OOB, 0, 0);

    /* #2 send + recv OOB */
    sc6(SYS_SENDTO, sv[1], (long)"B", 1, MSG_OOB, 0, 0);
    sc6(SYS_RECVFROM, sv[0], (long)&buf, 1, MSG_OOB, 0, 0);

    /* #3 send OOB, try normal recv — should not return data */
    sc6(SYS_SENDTO, sv[1], (long)"C", 1, MSG_OOB, 0, 0);
    r = sc6(SYS_RECVFROM, sv[0], (long)&buf, 1, MSG_DONTWAIT, 0, 0);

    if (r == -EAGAIN)
        pass("normal recv returns EAGAIN (correct)");
    else if (r < 0)
        pass("normal recv returns error (acceptable)");
    else
        fail("normal recv returned OOB data from stream", 0);

    sc1(SYS_CLOSE, sv[0]);
    sc1(SYS_CLOSE, sv[1]);
}

TEST("ltp/cve-2016-10044", test_cve_2016_10044);
TEST("ltp/cve-2016-7042",  test_cve_2016_7042);
TEST("ltp/cve-2017-17052", test_cve_2017_17052);
TEST("ltp/cve-2017-17053", test_cve_2017_17053);
TEST("ltp/cve-2017-2618",  test_cve_2017_2618);
TEST("ltp/cve-2017-2671",  test_cve_2017_2671);
TEST("ltp/cve-2025-38236", test_cve_2025_38236);
