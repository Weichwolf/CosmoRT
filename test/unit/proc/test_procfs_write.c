/* /proc generic write-path tests.
 *
 * Deckt die procfs_write-Infrastruktur und /proc/sys/fs/pipe-max-size ab.
 * Spiegelt LTP fcntl35: unprivileged pipe() wird auf pipe-max-size gecappt,
 * root bleibt beim Default. */
#include "ktest.h"

#define SYS_SETUID        105
#define F_GETPIPE_SZ_LOC  1032
#define NOBODY_UID        65534
#define PIPE_DEFAULT_SZ   65536

#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

static long read_file_int(const char *path) {
    long fd = sc3(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) return fd;
    char buf[32] = {0};
    long r = sc3(SYS_READ, fd, (long)buf, 31);
    sc1(SYS_CLOSE, fd);
    if (r <= 0) return -1;
    long v = 0; int i = 0;
    while (i < r && buf[i] >= '0' && buf[i] <= '9') {
        v = v * 10 + (buf[i] - '0');
        i++;
    }
    return v;
}

static long write_file_str(const char *path, const char *s, int len) {
    long fd = sc3(SYS_OPEN, (long)path, O_WRONLY, 0);
    if (fd < 0) return fd;
    long r = sc3(SYS_WRITE, fd, (long)s, len);
    sc1(SYS_CLOSE, fd);
    return r;
}

static void test_procfs_write(void) {
    puts("\n[procfs/write]\n");

    /* Default wert lesen — muss Linux-Default entsprechen. */
    long def = read_file_int("/proc/sys/fs/pipe-max-size");
    check("pipe-max-size readable", def > 0);
    check("pipe-max-size default 1MB", def == 1048576);

    /* Read-only ohne Write-Handler bleibt EACCES. */
    long fd = sc3(SYS_OPEN, (long)"/proc/meminfo", O_WRONLY, 0);
    check_val("meminfo O_WRONLY EACCES", fd, -EACCES);

    /* Schreiben eines validen Wertes. */
    const char *s4k = "4096\n";
    long w = write_file_str("/proc/sys/fs/pipe-max-size", s4k, 5);
    check_val("write 4096 -> consumed 5", w, 5);
    long got = read_file_int("/proc/sys/fs/pipe-max-size");
    check_val("readback 4096", got, 4096);

    /* Zu kleiner Wert -> EINVAL. */
    const char *too_small = "100\n";
    w = write_file_str("/proc/sys/fs/pipe-max-size", too_small, 4);
    check_val("write 100 -> EINVAL", w, -EINVAL);

    /* Negativ -> EINVAL. */
    const char *neg = "-5\n";
    w = write_file_str("/proc/sys/fs/pipe-max-size", neg, 3);
    check_val("write -5 -> EINVAL", w, -EINVAL);

    /* Nicht-parsbar -> EINVAL. */
    const char *junk = "abc\n";
    w = write_file_str("/proc/sys/fs/pipe-max-size", junk, 4);
    check_val("write abc -> EINVAL", w, -EINVAL);

    /* Root (euid==0) bleibt beim Default trotz Cap. */
    int fds[2];
    long r = sc1(SYS_PIPE, (long)fds);
    check_val("root pipe()", r, 0);
    if (r == 0) {
        long sz = sc2(SYS_FCNTL, fds[1], F_GETPIPE_SZ_LOC);
        check("root pipe size == default", sz == PIPE_DEFAULT_SZ);
        sc1(SYS_CLOSE, fds[0]);
        sc1(SYS_CLOSE, fds[1]);
    }

    /* Unprivileged Child: pipe() Initialkapazitaet == pipe-max-size (4096). */
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_SETUID, NOBODY_UID);
        int cfds[2];
        long cr = sc1(SYS_PIPE, (long)cfds);
        if (cr != 0) sc1(SYS_EXIT, 10);
        long sz = sc2(SYS_FCNTL, cfds[1], F_GETPIPE_SZ_LOC);
        sc1(SYS_CLOSE, cfds[0]);
        sc1(SYS_CLOSE, cfds[1]);
        sc1(SYS_EXIT, sz == 4096 ? 0 : 20 + (int)(sz / 4096));
    }
    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check_val("unpriv pipe capped at 4096", WEXITSTATUS(status), 0);

    /* Default zurueck — sonst polluted der Value den Rest der Suite. */
    const char *sdef = "1048576\n";
    w = write_file_str("/proc/sys/fs/pipe-max-size", sdef, 8);
    check_val("restore 1MB", w, 8);
    got = read_file_int("/proc/sys/fs/pipe-max-size");
    check_val("readback 1MB", got, 1048576);
}

TEST("procfs write", test_procfs_write);
