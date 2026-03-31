/* LTP add_key01-04 — ported to ktest */
#include "ktest.h"

/* CosmoRT has no keyring support. add_key returns -ENOSYS. */

static void test_add_key_enosys(void) {
    puts("\n[ltp/add_key01]\n");

    long r = sc5(SYS_ADD_KEY, (long)"user", (long)"test_key",
                 (long)"payload", 7, 0 /* KEY_SPEC_SESSION_KEYRING = -3, but 0 is fine for ENOSYS */);
    check_val("add_key ENOSYS", r, -ENOSYS);
}

TEST("ltp/add_key01", test_add_key_enosys);
