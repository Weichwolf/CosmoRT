#include "ktest.h"

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
