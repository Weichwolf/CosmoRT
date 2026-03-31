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

TEST("ltp/capget01",              test_capget01);
TEST("ltp/capget02-einval-ver",   test_capget02_einval_version);
TEST("ltp/capget02-einval-pid",   test_capget02_einval_pid);
TEST("ltp/capget02-esrch",        test_capget02_esrch);
TEST("ltp/capset01",              test_capset01);
TEST("ltp/capset02-einval",       test_capset02_einval);
TEST("ltp/capset03-efault",       test_capset03_efault);
