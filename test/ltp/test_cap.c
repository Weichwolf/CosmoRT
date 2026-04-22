/* LTP capget01-02 / capset01-04 — ported to ktest */
#include "ktest.h"

#define LINUX_CAPABILITY_VERSION_1  0x19980330
#define LINUX_CAPABILITY_VERSION_2  0x20071026
#define LINUX_CAPABILITY_VERSION_3  0x20080522

struct cap_header {
    uint32_t version;
    int      pid;
};

struct cap_data {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
};

/* ── capget01: basic capget with all three versions ── */

static void test_capget_version(uint32_t ver, const char *label) {
    struct cap_header hdr = { .version = ver, .pid = 0 };
    struct cap_data data[2];
    for (int i = 0; i < (int)sizeof(data); i++)
        ((char *)&data)[i] = 0;

    long r = sc2(SYS_CAPGET, (long)&hdr, (long)data);
    check_val(label, r, 0);
}

static void test_capget01(void) {
    puts("\n[ltp/capget01]\n");
    test_capget_version(LINUX_CAPABILITY_VERSION_1, "capget v1");
    test_capget_version(LINUX_CAPABILITY_VERSION_2, "capget v2");
    test_capget_version(LINUX_CAPABILITY_VERSION_3, "capget v3");
}

/* ── capget02: error cases ── */

static void test_capget02_einval_version(void) {
    puts("\n[ltp/capget02-einval-version]\n");

    struct cap_header hdr = { .version = 0, .pid = 0 };
    struct cap_data data[2];
    for (int i = 0; i < (int)sizeof(data); i++)
        ((char *)&data)[i] = 0;

    long r = sc2(SYS_CAPGET, (long)&hdr, (long)data);
    check_val("bad version EINVAL", r, -EINVAL);

    /* kernel should write preferred version back */
    check_val("kernel sets v3", (long)hdr.version, (long)LINUX_CAPABILITY_VERSION_3);
}

static void test_capget02_einval_pid(void) {
    puts("\n[ltp/capget02-einval-pid]\n");

    struct cap_header hdr = { .version = LINUX_CAPABILITY_VERSION_3, .pid = -1 };
    struct cap_data data[2];
    for (int i = 0; i < (int)sizeof(data); i++)
        ((char *)&data)[i] = 0;

    long r = sc2(SYS_CAPGET, (long)&hdr, (long)data);
    check_val("pid=-1 EINVAL", r, -EINVAL);
}

static void test_capget02_esrch(void) {
    puts("\n[ltp/capget02-esrch]\n");

    /* use a very high pid unlikely to exist */
    struct cap_header hdr = { .version = LINUX_CAPABILITY_VERSION_3, .pid = 999999 };
    struct cap_data data[2];
    for (int i = 0; i < (int)sizeof(data); i++)
        ((char *)&data)[i] = 0;

    long r = sc2(SYS_CAPGET, (long)&hdr, (long)data);
    check_val("nonexistent pid ESRCH", r, -ESRCH);
}

/* ── capset01: basic capset with version 3 ── */

static void test_capset01(void) {
    puts("\n[ltp/capset01]\n");

    /* first get current caps */
    struct cap_header hdr = { .version = LINUX_CAPABILITY_VERSION_3, .pid = 0 };
    struct cap_data data[2];
    for (int i = 0; i < (int)sizeof(data); i++)
        ((char *)&data)[i] = 0;

    long r = sc2(SYS_CAPGET, (long)&hdr, (long)data);
    check_val("capget before set", r, 0);

    /* set same caps back */
    hdr.version = LINUX_CAPABILITY_VERSION_3;
    hdr.pid = 0;
    r = sc2(SYS_CAPSET, (long)&hdr, (long)data);
    check_val("capset same caps", r, 0);
}

/* ── capset02: EINVAL for bad version ── */

static void test_capset02_einval(void) {
    puts("\n[ltp/capset02-einval]\n");

    struct cap_header hdr = { .version = 0, .pid = 0 };
    struct cap_data data[2];
    for (int i = 0; i < (int)sizeof(data); i++)
        ((char *)&data)[i] = 0;

    long r = sc2(SYS_CAPSET, (long)&hdr, (long)data);
    check_val("bad version EINVAL", r, -EINVAL);
}

/* ── capset03: EFAULT for NULL data ── */

static void test_capset03_efault(void) {
    puts("\n[ltp/capset03-efault]\n");

    struct cap_header hdr = { .version = LINUX_CAPABILITY_VERSION_3, .pid = 0 };
    long r = sc2(SYS_CAPSET, (long)&hdr, 0);
    check_val("NULL data EFAULT", r, -EFAULT);
}

/* ── capset02-eperm: subset-check EPERM branches ─
 * Fork → set narrow pP, then try to widen / violate pE ⊆ pP / pI rules. */

#define CAP_KILL_BIT     5
#define CAP_SETPCAP_BIT  8
#define CAP_CHOWN_BIT    0
#define CAP_NET_RAW_BIT  13
#define CAP1 ((1u<<CAP_NET_RAW_BIT)|(1u<<CAP_CHOWN_BIT)|(1u<<CAP_SETPCAP_BIT))
#define CAP2 (CAP1 | (1u<<CAP_KILL_BIT))

static void test_capset02_eperm(void) {
    puts("\n[ltp/capset02-eperm]\n");

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct cap_header hdr = { .version = LINUX_CAPABILITY_VERSION_3, .pid = 0 };
        struct cap_data data[2];
        for (int i = 0; i < (int)sizeof(data); i++) ((char *)&data)[i] = 0;
        data[0].effective = CAP1; data[0].permitted = CAP1; data[0].inheritable = CAP1;
        if (sc2(SYS_CAPSET, (long)&hdr, (long)data) != 0) sc1(SYS_EXIT, 10);

        if (sc5(SYS_PRCTL, 24, CAP_KILL_BIT, 0, 0, 0) != 0) sc1(SYS_EXIT, 11);

        data[0].effective = CAP2; data[0].permitted = CAP1; data[0].inheritable = CAP1;
        if (sc2(SYS_CAPSET, (long)&hdr, (long)data) != -EPERM) sc1(SYS_EXIT, 20);

        data[0].effective = CAP1; data[0].permitted = CAP2; data[0].inheritable = CAP1;
        if (sc2(SYS_CAPSET, (long)&hdr, (long)data) != -EPERM) sc1(SYS_EXIT, 21);

        data[0].effective = CAP1; data[0].permitted = CAP1; data[0].inheritable = CAP2;
        if (sc2(SYS_CAPSET, (long)&hdr, (long)data) != -EPERM) sc1(SYS_EXIT, 22);

        sc1(SYS_EXIT, 0);
    }
    check("fork", pid > 0);
    if (pid > 0) {
        int status = 0;
        sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
        int code = (status >> 8) & 0xFF;
        check_val("capset subset checks", (long)code, 0);
    }
}

/* ── capset03-inh: pI new ⊆ old pI ∪ (old pP ∩ bounding) ── */

static void test_capset03_inh(void) {
    puts("\n[ltp/capset03-inh]\n");

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct cap_header hdr = { .version = LINUX_CAPABILITY_VERSION_3, .pid = 0 };
        struct cap_data data[2];
        for (int i = 0; i < (int)sizeof(data); i++) ((char *)&data)[i] = 0;
        uint32_t only_kill = 1u << CAP_KILL_BIT;
        data[0].effective = only_kill; data[0].permitted = only_kill;
        data[0].inheritable = only_kill;
        if (sc2(SYS_CAPSET, (long)&hdr, (long)data) != 0) sc1(SYS_EXIT, 10);

        data[0].inheritable = only_kill | (1u << CAP_NET_RAW_BIT);
        if (sc2(SYS_CAPSET, (long)&hdr, (long)data) != -EPERM) sc1(SYS_EXIT, 20);

        sc1(SYS_EXIT, 0);
    }
    check("fork", pid > 0);
    if (pid > 0) {
        int status = 0;
        sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
        int code = (status >> 8) & 0xFF;
        check_val("pI subset rule", (long)code, 0);
    }
}

/* ── prctl PR_CAPBSET_READ (23) / PR_CAPBSET_DROP (24) ── */

static void test_capbset(void) {
    puts("\n[ltp/capbset]\n");

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        long r = sc5(SYS_PRCTL, 23, CAP_KILL_BIT, 0, 0, 0);
        if (r != 1) sc1(SYS_EXIT, 10);

        if (sc5(SYS_PRCTL, 24, CAP_KILL_BIT, 0, 0, 0) != 0) sc1(SYS_EXIT, 11);

        if (sc5(SYS_PRCTL, 23, CAP_KILL_BIT, 0, 0, 0) != 0) sc1(SYS_EXIT, 12);

        if (sc5(SYS_PRCTL, 24, 41, 0, 0, 0) != -EINVAL) sc1(SYS_EXIT, 13);

        sc1(SYS_EXIT, 0);
    }
    check("fork", pid > 0);
    if (pid > 0) {
        int status = 0;
        sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
        int code = (status >> 8) & 0xFF;
        check_val("capbset ops", (long)code, 0);
    }
}

/* ── setuid clears capability sets (Linux cap_emulate_setxuid) ── */
static void test_caps_setxuid_clear(void) {
    puts("\n[ltp/caps-setxuid-clear]\n");

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct cap_header hdr = { .version = LINUX_CAPABILITY_VERSION_3, .pid = 0 };
        struct cap_data data[2];
        for (int i = 0; i < (int)sizeof(data); i++) ((char *)&data)[i] = 0;

        /* setresuid(1000, 1000, 1000) from root → non-root everywhere */
        if (sc3(SYS_SETRESUID, 1000, 1000, 1000) != 0) sc1(SYS_EXIT, 10);

        sc2(SYS_CAPGET, (long)&hdr, (long)data);
        if (data[0].effective != 0) sc1(SYS_EXIT, 20);
        if (data[0].permitted != 0) sc1(SYS_EXIT, 21);
        sc1(SYS_EXIT, 0);
    }
    check("fork", pid > 0);
    if (pid > 0) {
        int status = 0;
        sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
        int code = (status >> 8) & 0xFF;
        check_val("setuid clears caps", (long)code, 0);
    }
}

TEST("ltp/caps-setxuid-clear",    test_caps_setxuid_clear);
TEST("ltp/capget01",              test_capget01);
TEST("ltp/capget02-einval-ver",   test_capget02_einval_version);
TEST("ltp/capget02-einval-pid",   test_capget02_einval_pid);
TEST("ltp/capget02-esrch",        test_capget02_esrch);
TEST("ltp/capset01",              test_capset01);
TEST("ltp/capset02-einval",       test_capset02_einval);
TEST("ltp/capset02-eperm",        test_capset02_eperm);
TEST("ltp/capset03-efault",       test_capset03_efault);
TEST("ltp/capset03-inh",          test_capset03_inh);
TEST("ltp/capbset",               test_capbset);
