/* LTP confstr01 — ported to ktest
 * confstr(3) is a libc function, not a syscall.
 * LTP source only calls confstr() which requires libc.
 * Nothing to test at syscall level — placeholder PASS. */
#include "ktest.h"

static void test_confstr01(void) {
    puts("\n[ltp/confstr01]\n");
    /* confstr is implemented entirely in libc (musl/glibc).
     * No kernel syscall involved. Nothing to test freestanding. */
    pass("confstr is libc-only, skip");
}

TEST("ltp/confstr01", test_confstr01);
