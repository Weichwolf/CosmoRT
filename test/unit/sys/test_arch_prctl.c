#include "ktest.h"

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

#define NONCANONICAL_USER_VA 0x0000800000000000UL

/* Userspace canary that the kernel must preserve across schedule() / fork(). */
static uint64_t gs_storage;

static void test_arch_get_set_gs(void) {
    puts("\n[ARCH_SET_GS / ARCH_GET_GS]\n");

    /* Initial state: child of init inherits gs_base = 0 */
    uint64_t val = 0xDEAD;
    long r = sc2(SYS_ARCH_PRCTL, ARCH_GET_GS, (long)&val);
    check_val("ARCH_GET_GS returns 0", r, 0);
    check_val("initial gs_base == 0", (long)val, 0);

    /* Set a canonical user-space address, read it back. */
    gs_storage = 0xCAFE1234;
    uint64_t want = (uint64_t)(unsigned long)&gs_storage;
    r = sc2(SYS_ARCH_PRCTL, ARCH_SET_GS, (long)want);
    check_val("ARCH_SET_GS canonical addr returns 0", r, 0);

    val = 0;
    r = sc2(SYS_ARCH_PRCTL, ARCH_GET_GS, (long)&val);
    check_val("ARCH_GET_GS returns 0", r, 0);
    check_val("gs_base round-trip", (long)val, (long)want);

    /* Non-canonical => -EPERM (Linux ABI) */
    r = sc2(SYS_ARCH_PRCTL, ARCH_SET_GS, (long)NONCANONICAL_USER_VA);
    check_val("ARCH_SET_GS non-canonical -> -EPERM", r, -EPERM);

    /* gs_base must still be the previous canonical value */
    val = 0;
    sc2(SYS_ARCH_PRCTL, ARCH_GET_GS, (long)&val);
    check_val("gs_base unchanged after EPERM", (long)val, (long)want);

    /* Reset to 0 so later tests don't see stray gs */
    sc2(SYS_ARCH_PRCTL, ARCH_SET_GS, 0);
}

static void test_arch_set_fs_noncanonical(void) {
    puts("\n[ARCH_SET_FS canonical-form]\n");
    /* ARCH_SET_FS rejects non-canonical addresses with -EPERM. */
    long r = sc2(SYS_ARCH_PRCTL, ARCH_SET_FS, (long)NONCANONICAL_USER_VA);
    check_val("ARCH_SET_FS non-canonical -> -EPERM", r, -EPERM);
}

TEST("ARCH_SET_GS / ARCH_GET_GS", test_arch_get_set_gs);
TEST("ARCH_SET_FS canonical-form", test_arch_set_fs_noncanonical);
