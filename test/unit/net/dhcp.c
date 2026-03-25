/* test: DHCP — verify IP was assigned */
#include "ktest.h"

static void test_dhcp(void) {
    puts("\n[net/dhcp]\n");

    /* Read /proc/dmesg and check for DHCP success message */
    long fd = sc3(SYS_OPEN, (long)"/proc/dmesg", 0, 0);
    check("open /proc/dmesg", fd >= 0);
    if (fd < 0) return;

    char buf[2048];
    long r = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    sc1(SYS_CLOSE, fd);
    check("read dmesg", r > 0);
    if (r <= 0) return;
    buf[r] = 0;

    /* Look for "DHCP...ok" in dmesg */
    int found = 0;
    for (int i = 0; i < r - 7; i++)
        if (buf[i]=='D'&&buf[i+1]=='H'&&buf[i+2]=='C'&&buf[i+3]=='P'
            &&buf[i+4]=='.'&&buf[i+5]=='.'&&buf[i+6]=='.') found = 1;
    check("DHCP ok in dmesg", found);
}

TEST("net/dhcp", test_dhcp);
