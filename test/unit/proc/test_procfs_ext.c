#include "ktest.h"

static int contains(const char *hay, int haylen, const char *needle) {
    int nlen = 0;
    while (needle[nlen]) nlen++;
    for (int i = 0; i <= haylen - nlen; i++) {
        int j = 0;
        while (j < nlen && hay[i + j] == needle[j]) j++;
        if (j == nlen) return 1;
    }
    return 0;
}

static int starts_with(const char *s, const char *prefix) {
    while (*prefix) { if (*s++ != *prefix++) return 0; }
    return 1;
}

static void test_procfs_ext(void) {
    puts("\n[Procfs Ext]\n");

    /* /proc/self/exe — readlink returns a non-empty path */
    {
        char buf[256] = {0};
        long r = sc3(SYS_READLINK, (long)"/proc/self/exe", (long)buf, 255);
        check("readlink /proc/self/exe > 0", r > 0);
        if (r > 0) {
            buf[r] = 0;
            /* Path should start with '/' */
            check("exe is absolute path", buf[0] == '/');
        }
    }

    /* /proc/self/cmdline — non-empty, null-separated */
    {
        long fd = sc3(SYS_OPEN, (long)"/proc/self/cmdline", O_RDONLY, 0);
        check("open /proc/self/cmdline", fd >= 0);
        if (fd >= 0) {
            char buf[256] = {0};
            long r = sc3(SYS_READ, fd, (long)buf, 255);
            check("read cmdline > 0", r > 0);
            sc1(SYS_CLOSE, fd);
        }
    }

    /* /proc/self/stat — starts with PID */
    {
        long fd = sc3(SYS_OPEN, (long)"/proc/self/stat", O_RDONLY, 0);
        check("open /proc/self/stat", fd >= 0);
        if (fd >= 0) {
            char buf[256] = {0};
            long r = sc3(SYS_READ, fd, (long)buf, 255);
            check("read stat > 0", r > 0);
            if (r > 0) {
                /* First char should be a digit (PID) */
                check("stat starts with digit", buf[0] >= '0' && buf[0] <= '9');
                /* Should contain comm in parens */
                check("stat contains parens", contains(buf, (int)r, "("));
            }
            sc1(SYS_CLOSE, fd);
        }
    }

    /* /proc/meminfo — contains MemTotal */
    {
        long fd = sc3(SYS_OPEN, (long)"/proc/meminfo", O_RDONLY, 0);
        check("open /proc/meminfo", fd >= 0);
        if (fd >= 0) {
            char buf[512] = {0};
            long r = sc3(SYS_READ, fd, (long)buf, 511);
            check("meminfo > 0", r > 0);
            if (r > 0)
                check("meminfo has MemTotal", starts_with(buf, "MemTotal:"));
            sc1(SYS_CLOSE, fd);
        }
    }

    /* /proc/self/maps — contains hex addresses (format: <hex>-<hex>) */
    {
        long fd = sc3(SYS_OPEN, (long)"/proc/self/maps", O_RDONLY, 0);
        check("open /proc/self/maps", fd >= 0);
        if (fd >= 0) {
            char buf[512] = {0};
            long r = sc3(SYS_READ, fd, (long)buf, 511);
            check("maps > 0", r > 0);
            if (r > 0) {
                /* Should contain a dash (addr range separator) */
                check("maps has addr range", contains(buf, (int)r, "-"));
                /* Should contain permission chars */
                int has_perm = contains(buf, (int)r, "r-x") ||
                               contains(buf, (int)r, "rw-") ||
                               contains(buf, (int)r, "r--") ||
                               contains(buf, (int)r, "rwx");
                check("maps has perms", has_perm);
            }
            sc1(SYS_CLOSE, fd);
        }
    }
}

TEST("procfs_ext", test_procfs_ext);
