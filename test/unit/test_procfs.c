#include "ktest.h"

static int kstrncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 1;
        if (!a[i]) return 0;
    }
    return 0;
}

static void test_procfs(void) {
    puts("\n[Procfs]\n");

    /* /proc/dmesg — should contain boot output */
    long fd = sc3(SYS_open, (long)"/proc/dmesg", O_RDONLY, 0);
    check("open /proc/dmesg", fd >= 0);
    if (fd >= 0) {
        char buf[128] = {0};
        long r = sc3(SYS_read, fd, (long)buf, 127);
        check("read /proc/dmesg > 0", r > 0);
        /* Boot output starts with \r\n\r\nCosmoRT */
        int found = 0;
        for (int i = 0; i < (int)r - 7; i++) {
            if (buf[i]=='C' && buf[i+1]=='o' && buf[i+2]=='s' &&
                buf[i+3]=='m' && buf[i+4]=='o' && buf[i+5]=='R' && buf[i+6]=='T') {
                found = 1; break;
            }
        }
        check("dmesg contains CosmoRT", found);
        sc1(SYS_close, fd);
    }

    /* /proc/meminfo — should contain MemTotal */
    fd = sc3(SYS_open, (long)"/proc/meminfo", O_RDONLY, 0);
    check("open /proc/meminfo", fd >= 0);
    if (fd >= 0) {
        char buf[256] = {0};
        long r = sc3(SYS_read, fd, (long)buf, 255);
        check("read /proc/meminfo > 0", r > 0);
        check("meminfo starts with MemTotal", r >= 9 && kstrncmp(buf, "MemTotal:", 9) == 0);
        sc1(SYS_close, fd);
    }

    /* /proc/cpuinfo — should contain cores */
    fd = sc3(SYS_open, (long)"/proc/cpuinfo", O_RDONLY, 0);
    check("open /proc/cpuinfo", fd >= 0);
    if (fd >= 0) {
        char buf[256] = {0};
        long r = sc3(SYS_read, fd, (long)buf, 255);
        check("read /proc/cpuinfo > 0", r > 0);
        check("cpuinfo starts with cores", r >= 6 && kstrncmp(buf, "cores:", 6) == 0);
        sc1(SYS_close, fd);
    }

    /* /proc/nonexistent — should fail */
    fd = sc3(SYS_open, (long)"/proc/nonexistent", O_RDONLY, 0);
    check("open /proc/nonexistent fails", fd < 0);
}

TEST("procfs", test_procfs);
