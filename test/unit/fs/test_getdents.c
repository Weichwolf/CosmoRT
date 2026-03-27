#include "ktest.h"

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[1];
};

#define DT_DIR 4
#define DT_REG 8

static int kstrcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void test_getdents(void) {
    puts("\n[getdents64]\n");

    /* open("/") and list entries — should find usr, home, tmp (CosmoFS or ramfs) */
    long fd = sc3(SYS_OPEN, (long)"/", O_RDONLY | O_DIRECTORY, 0);
    check("open / for getdents", fd >= 0);
    if (fd >= 0) {
        char buf[1024];
        long n = sc3(SYS_GETDENTS64, fd, (long)buf, sizeof(buf));
        check("getdents64 / > 0", n > 0);

        /* Scan for known root entries */
        int found_home = 0, found_tmp = 0;
        long off = 0;
        while (off < n) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
            if (d->d_reclen == 0) break;
            if (kstrcmp(d->d_name, "home") == 0) found_home = 1;
            if (kstrcmp(d->d_name, "tmp") == 0) found_tmp = 1;
            off += d->d_reclen;
        }
        check("root has home", found_home);
        check("root has tmp", found_tmp);
        sc1(SYS_CLOSE, fd);
    }

    /* open("/proc") and list entries — should find dmesg, meminfo */
    fd = sc3(SYS_OPEN, (long)"/proc", O_RDONLY | O_DIRECTORY, 0);
    check("open /proc for getdents", fd >= 0);
    if (fd >= 0) {
        char buf[1024];
        long n = sc3(SYS_GETDENTS64, fd, (long)buf, sizeof(buf));
        check("getdents64 /proc > 0", n > 0);

        int found_dmesg = 0, found_meminfo = 0;
        long off = 0;
        while (off < n) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
            if (d->d_reclen == 0) break;
            if (kstrcmp(d->d_name, "dmesg") == 0) found_dmesg = 1;
            if (kstrcmp(d->d_name, "meminfo") == 0) found_meminfo = 1;
            off += d->d_reclen;
        }
        check("/proc has dmesg", found_dmesg);
        check("/proc has meminfo", found_meminfo);
        sc1(SYS_CLOSE, fd);
    }

    /* getdents on non-directory should fail */
    fd = sc3(SYS_OPEN, (long)"/proc/dmesg", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[256];
        long n = sc3(SYS_GETDENTS64, fd, (long)buf, sizeof(buf));
        check("getdents on file = ENOTDIR", n == -ENOTDIR);
        sc1(SYS_CLOSE, fd);
    }

    /* Second call after consuming all entries returns 0 */
    fd = sc3(SYS_OPEN, (long)"/proc", O_RDONLY | O_DIRECTORY, 0);
    if (fd >= 0) {
        char buf[2048];
        long n1 = sc3(SYS_GETDENTS64, fd, (long)buf, sizeof(buf));
        if (n1 > 0) {
            long n2 = sc3(SYS_GETDENTS64, fd, (long)buf, sizeof(buf));
            check("getdents64 EOF returns 0", n2 == 0);
        }
        sc1(SYS_CLOSE, fd);
    }
}

TEST("getdents64", test_getdents);
