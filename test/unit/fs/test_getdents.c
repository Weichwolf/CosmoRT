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

/* Count entries returned by getdents64 in buf of n bytes */
static int count_dirents(const char *buf, long n) {
    int count = 0;
    long off = 0;
    while (off < n) {
        struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
        if (d->d_reclen == 0) break;
        count++;
        off += d->d_reclen;
    }
    return count;
}

/* Search for a name in getdents64 output */
static int find_name(const char *buf, long n, const char *name) {
    long off = 0;
    while (off < n) {
        struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
        if (d->d_reclen == 0) break;
        if (kstrcmp(d->d_name, name) == 0) return 1;
        off += d->d_reclen;
    }
    return 0;
}

/* Read all entries via multiple getdents64 calls (loop until EOF).
 * Returns total bytes in out_buf, fills entry_count. */
static long getdents_all(int fd, char *out_buf, int buf_cap, int call_buf_size,
                         int *entry_count) {
    long total = 0;
    *entry_count = 0;
    /* Temporary buffer for each getdents call */
    char tmp[256]; /* small buffer to force multiple calls */
    int use_size = call_buf_size < (int)sizeof(tmp) ? call_buf_size : (int)sizeof(tmp);

    while (total < buf_cap) {
        long n = sc3(SYS_GETDENTS64, fd, (long)tmp, use_size);
        if (n <= 0) break;
        /* Copy to output buffer */
        long off = 0;
        while (off < n) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(tmp + off);
            if (d->d_reclen == 0) break;
            if (total + d->d_reclen <= buf_cap) {
                for (int i = 0; i < d->d_reclen; i++)
                    out_buf[total + i] = tmp[off + i];
                total += d->d_reclen;
                (*entry_count)++;
            }
            off += d->d_reclen;
        }
    }
    return total;
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

    /* Create directory with 10 files and verify all 10 + . + .. appear */
    sc2(SYS_MKDIR, (long)"/tmp/dirtest", 0755);
    const char *names[] = {
        "alpha", "bravo", "charlie", "delta", "echo",
        "foxtrot", "golf", "hotel", "india", "juliet"
    };
    for (int i = 0; i < 10; i++) {
        char path[64];
        /* Build "/tmp/dirtest/<name>" */
        int p = 0;
        const char *pfx = "/tmp/dirtest/";
        while (*pfx) path[p++] = *pfx++;
        const char *nm = names[i];
        while (*nm) path[p++] = *nm++;
        path[p] = 0;

        long f = sc3(SYS_OPEN, (long)path, O_CREAT | O_WRONLY, 0644);
        if (f >= 0) sc1(SYS_CLOSE, f);
    }

    /* Single large-buffer read — should get all 12 entries (10 files + . + ..) */
    fd = sc3(SYS_OPEN, (long)"/tmp/dirtest", O_RDONLY | O_DIRECTORY, 0);
    check("open /tmp/dirtest", fd >= 0);
    if (fd >= 0) {
        char buf[4096];
        long n = sc3(SYS_GETDENTS64, fd, (long)buf, sizeof(buf));
        check("getdents64 dirtest > 0", n > 0);
        int cnt = count_dirents(buf, n);
        check_ge("dirtest entry count", cnt, 10); /* 10 files (+ . + .. on ext2) */

        /* Verify all names present */
        for (int i = 0; i < 10; i++)
            check(names[i], find_name(buf, n, names[i]));

        sc1(SYS_CLOSE, fd);
    }

    /* Multi-call read with small buffer — verify all entries still appear */
    fd = sc3(SYS_OPEN, (long)"/tmp/dirtest", O_RDONLY | O_DIRECTORY, 0);
    if (fd >= 0) {
        char all_buf[4096];
        int total_entries = 0;
        long total_bytes = getdents_all((int)fd, all_buf, (int)sizeof(all_buf),
                                         128, &total_entries);
        check("multi-call getdents > 0", total_bytes > 0);
        check_ge("multi-call entry count", total_entries, 10);

        /* Verify last entries aren't dropped */
        for (int i = 0; i < 10; i++)
            check(names[i], find_name(all_buf, total_bytes, names[i]));

        sc1(SYS_CLOSE, fd);
    }

    /* d_off values should be strictly increasing */
    fd = sc3(SYS_OPEN, (long)"/tmp/dirtest", O_RDONLY | O_DIRECTORY, 0);
    if (fd >= 0) {
        char buf[4096];
        long n = sc3(SYS_GETDENTS64, fd, (long)buf, sizeof(buf));
        int64_t prev_off = -1;
        int monotonic = 1;
        long off = 0;
        while (off < n) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
            if (d->d_reclen == 0) break;
            if (d->d_off <= prev_off) monotonic = 0;
            prev_off = d->d_off;
            off += d->d_reclen;
        }
        check("d_off monotonically increasing", monotonic);
        sc1(SYS_CLOSE, fd);
    }
}

TEST("getdents64", test_getdents);
