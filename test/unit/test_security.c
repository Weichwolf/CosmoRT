#include "ktest.h"

static void test_security(void) {
    puts("\n[Security]\n");

    /* User pointer validation: kernel address should be rejected */
    long r = sc3(SYS_write, 1, 0xFFFF800000000000LL, 10);
    check("write(kernel_addr) → EFAULT", r == -14);

    r = sc3(SYS_read, 0, 0xFFFF800000000000LL, 10);
    check("read(kernel_addr) → EFAULT", r == -14);

    /* Overflow check */
    r = sc6(SYS_mmap, 0, (long)-1, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap(SIZE_MAX) fails", r < 0);
}

TEST("security", test_security);
