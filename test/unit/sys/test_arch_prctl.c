#include "ktest.h"

/* ── ARCH_GET_GS returns 0 (known limitation: GS unused) ── */

#define ARCH_SET_GS 0x1001
#define ARCH_GET_GS 0x1004

static void test_arch_get_gs(void) {
    puts("\n[ARCH_GET_GS]\n");

    uint64_t val = 0xDEAD;
    long r = sc2(SYS_ARCH_PRCTL, ARCH_GET_GS, (long)&val);
    check_val("arch_prctl ARCH_GET_GS returns 0", r, 0);
    check_val("GS base = 0 (unused)", (long)val, 0);

    /* ARCH_SET_GS still returns -EINVAL (unsupported) */
    r = sc2(SYS_ARCH_PRCTL, ARCH_SET_GS, 0x1000);
    check_val("arch_prctl ARCH_SET_GS → -EINVAL", r, -EINVAL);
}

TEST("ARCH_GET_GS", test_arch_get_gs);
