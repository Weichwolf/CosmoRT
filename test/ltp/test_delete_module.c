/* LTP delete_module02 — ported to ktest */
#include "ktest.h"

/* CosmoRT has no module support. All module syscalls return -ENOSYS. */

static void test_delete_module_enosys(void) {
    puts("\n[ltp/delete_module02]\n");

    long r = sc2(SYS_DELETE_MODULE, (long)"dummy_module", 0);
    check_val("delete_module ENOSYS", r, -ENOSYS);
}

TEST("ltp/delete_module02", test_delete_module_enosys);
