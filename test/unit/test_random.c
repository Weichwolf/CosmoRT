#include "ktest.h"

static void test_random(void) {
    puts("\n[Random]\n");
    uint8_t buf[16] = {0};
    long r = sc3(SYS_getrandom, (long)buf, 16, 0);
    check_val("getrandom returns 16", r, 16);
    int nonzero = 0;
    for (int i = 0; i < 16; i++)
        if (buf[i]) nonzero++;
    check("getrandom produces data", nonzero > 0);
}

TEST("random", test_random);
