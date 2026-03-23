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

/* ── ELOOP: symlink loop detection (max 8 levels) ── */

static void test_eloop(void) {
    puts("\n[ELOOP]\n");

    /* Create circular symlinks: /tmp/loop_a → /tmp/loop_b, /tmp/loop_b → /tmp/loop_a */
    long r = sc2(SYS_SYMLINK, (long)"/tmp/loop_b", (long)"/tmp/loop_a");
    check_val("symlink /tmp/loop_a → /tmp/loop_b", r, 0);

    r = sc2(SYS_SYMLINK, (long)"/tmp/loop_a", (long)"/tmp/loop_b");
    check_val("symlink /tmp/loop_b → /tmp/loop_a", r, 0);

    /* open through circular symlink should fail with ELOOP */
    r = sc3(SYS_OPEN, (long)"/tmp/loop_a", O_RDONLY, 0);
    check("open symlink loop → error", r < 0);

    /* readlink on a non-looping symlink still works */
    r = sc2(SYS_SYMLINK, (long)"/tmp", (long)"/tmp/link_ok");
    check("symlink /tmp/link_ok", r == 0 || r == -EEXIST);

    char buf[128] = {0};
    r = sc3(SYS_READLINK, (long)"/tmp/link_ok", (long)buf, 127);
    check("readlink /tmp/link_ok succeeds", r > 0);

    /* Cleanup */
    sc1(SYS_UNLINK, (long)"/tmp/loop_a");
    sc1(SYS_UNLINK, (long)"/tmp/loop_b");
    sc1(SYS_UNLINK, (long)"/tmp/link_ok");
}

TEST("ELOOP", test_eloop);
