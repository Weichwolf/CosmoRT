#include "ktest.h"

/* ── /proc/cpuinfo Linux format ──────────────── */

static void test_proc_cpuinfo_linux(void) {
    puts("\n[/proc/cpuinfo format]\n");

    long fd = sc3(SYS_OPEN, (long)"/proc/cpuinfo", O_RDONLY, 0);
    check("open /proc/cpuinfo", fd >= 0);
    if (fd < 0) return;

    char buf[512] = {0};
    long r = sc3(SYS_READ, fd, (long)buf, 511);
    check("read cpuinfo > 0", r > 0);

    /* Should contain "processor\t: 0" */
    int found_proc = 0, found_model = 0, found_mhz = 0;
    for (int i = 0; i < (int)r - 10; i++) {
        if (buf[i]=='p' && buf[i+1]=='r' && buf[i+2]=='o' && buf[i+3]=='c' &&
            buf[i+4]=='e' && buf[i+5]=='s' && buf[i+6]=='s' && buf[i+7]=='o' &&
            buf[i+8]=='r') found_proc = 1;
        if (buf[i]=='m' && buf[i+1]=='o' && buf[i+2]=='d' && buf[i+3]=='e' &&
            buf[i+4]=='l') found_model = 1;
        if (buf[i]=='c' && buf[i+1]=='p' && buf[i+2]=='u' && buf[i+3]==' ' &&
            buf[i+4]=='M' && buf[i+5]=='H' && buf[i+6]=='z') found_mhz = 1;
    }
    check("has processor field", found_proc);
    check("has model name", found_model);
    check("has cpu MHz", found_mhz);

    sc1(SYS_CLOSE, fd);
}

TEST("/proc/cpuinfo format", test_proc_cpuinfo_linux);
