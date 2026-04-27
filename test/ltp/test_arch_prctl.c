/* LTP arch_prctl01 — ported to ktest */
#include "ktest.h"

/* ── arch_prctl01: ARCH_SET_FS/GET_FS and ARCH_SET_GS/GET_GS roundtrip ── */

static void test_arch_prctl_fs(void) {
    puts("\n[ltp/arch_prctl-fs]\n");

    /* get current FS base */
    unsigned long fs_orig = 0;
    long r = sc2(SYS_ARCH_PRCTL, ARCH_GET_FS, (long)&fs_orig);
    check_val("ARCH_GET_FS", r, 0);

    /* set a new FS base (use a stack variable address as safe value) */
    unsigned long new_fs = fs_orig;
    r = sc2(SYS_ARCH_PRCTL, ARCH_SET_FS, new_fs);
    check_val("ARCH_SET_FS", r, 0);

    /* verify roundtrip */
    unsigned long fs_read = 0;
    r = sc2(SYS_ARCH_PRCTL, ARCH_GET_FS, (long)&fs_read);
    check_val("ARCH_GET_FS after set", r, 0);
    check_val("FS roundtrip", (long)fs_read, (long)new_fs);

    /* restore original */
    sc2(SYS_ARCH_PRCTL, ARCH_SET_FS, fs_orig);
}

static void test_arch_prctl_gs(void) {
    puts("\n[ltp/arch_prctl-gs]\n");

    /* get current GS base */
    unsigned long gs_orig = 0;
    long r = sc2(SYS_ARCH_PRCTL, ARCH_GET_GS, (long)&gs_orig);
    check_val("ARCH_GET_GS", r, 0);

    /* set a known value */
    unsigned long new_gs = 0x12345000;
    r = sc2(SYS_ARCH_PRCTL, ARCH_SET_GS, new_gs);
    check_val("ARCH_SET_GS", r, 0);

    /* verify roundtrip */
    unsigned long gs_read = 0;
    r = sc2(SYS_ARCH_PRCTL, ARCH_GET_GS, (long)&gs_read);
    check_val("ARCH_GET_GS after set", r, 0);
    check_val("GS roundtrip", (long)gs_read, (long)new_gs);

    /* restore original */
    sc2(SYS_ARCH_PRCTL, ARCH_SET_GS, gs_orig);
}

static void test_arch_prctl_invalid(void) {
    puts("\n[ltp/arch_prctl-invalid]\n");

    /* invalid subcommand should return EINVAL */
    long r = sc2(SYS_ARCH_PRCTL, 0xFFFF, 0);
    check_val("invalid code EINVAL", r, -EINVAL);
}

TEST("ltp/arch_prctl-fs",      test_arch_prctl_fs);
TEST("ltp/arch_prctl-gs",      test_arch_prctl_gs);
TEST("ltp/arch_prctl-invalid", test_arch_prctl_invalid);
