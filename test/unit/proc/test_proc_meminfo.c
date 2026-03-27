#include "ktest.h"

/* ── /proc/meminfo Linux format ──────────────── */

static void test_proc_meminfo_linux(void) {
    puts("\n[/proc/meminfo format]\n");

    long fd = sc3(SYS_OPEN, (long)"/proc/meminfo", O_RDONLY, 0);
    check("open /proc/meminfo", fd >= 0);
    if (fd < 0) return;

    char buf[512] = {0};
    long r = sc3(SYS_READ, fd, (long)buf, 511);
    check("read meminfo > 0", r > 0);

    /* Should contain MemAvailable, Buffers, Cached */
    int found_avail = 0, found_buf = 0, found_cache = 0;
    for (int i = 0; i < (int)r - 8; i++) {
        if (buf[i]=='M' && buf[i+1]=='e' && buf[i+2]=='m' &&
            buf[i+3]=='A' && buf[i+4]=='v') found_avail = 1;
        if (buf[i]=='B' && buf[i+1]=='u' && buf[i+2]=='f' &&
            buf[i+3]=='f' && buf[i+4]=='e') found_buf = 1;
        if (buf[i]=='C' && buf[i+1]=='a' && buf[i+2]=='c' &&
            buf[i+3]=='h' && buf[i+4]=='e') found_cache = 1;
    }
    check("has MemAvailable", found_avail);
    check("has Buffers", found_buf);
    check("has Cached", found_cache);

    sc1(SYS_CLOSE, fd);
}

TEST("/proc/meminfo format", test_proc_meminfo_linux);
