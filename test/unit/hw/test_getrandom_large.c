#include "ktest.h"

/* ── getrandom large buffer tests ────────────── */

static void test_getrandom_large(void) {
    puts("\n[getrandom large]\n");

    /* 512 bytes — was previously capped at 256 */
    uint8_t buf[512];
    for (int i = 0; i < 512; i++) buf[i] = 0;
    long r = sc3(SYS_GETRANDOM, (long)buf, 512, 0);
    check_val("getrandom 512 returns 512", r, 512);

    /* Check second half has data (would be zero if capped at 256) */
    int nonzero_second = 0;
    for (int i = 256; i < 512; i++)
        if (buf[i]) nonzero_second++;
    check("getrandom second 256 bytes have data", nonzero_second > 0);

    /* 1024 bytes */
    uint8_t big[1024];
    for (int i = 0; i < 1024; i++) big[i] = 0;
    r = sc3(SYS_GETRANDOM, (long)big, 1024, 0);
    check_val("getrandom 1024 returns 1024", r, 1024);
}

TEST("getrandom large", test_getrandom_large);
