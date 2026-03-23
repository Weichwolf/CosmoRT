/* LTP: open/openat/close — flags, modes, error paths */
#define _GNU_SOURCE
#include "ltp.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

int main(void) {
    printf("=== LTP: open ===\n");

    /* 1. open /dev/null readable */
    int fd = open("/dev/null", O_RDONLY);
    CHECK("open /dev/null O_RDONLY", fd >= 0);
    if (fd >= 0) close(fd);

    /* 2. open non-existent → ENOENT */
    errno = 0;
    fd = open("/nonexistent_path_xyz", O_RDONLY);
    CHECK_ERRNO("open ENOENT", fd == -1, ENOENT);

    /* 3. O_CREAT | O_EXCL creates new file */
    const char *tmp = "/tmp/ltp_open_test";
    unlink(tmp);
    fd = open(tmp, O_CREAT | O_EXCL | O_WRONLY, 0644);
    CHECK("open O_CREAT|O_EXCL new", fd >= 0);
    if (fd >= 0) close(fd);

    /* 4. O_CREAT | O_EXCL on existing → EEXIST */
    errno = 0;
    fd = open(tmp, O_CREAT | O_EXCL | O_WRONLY, 0644);
    CHECK_ERRNO("open O_CREAT|O_EXCL exists → EEXIST", fd == -1, EEXIST);
    unlink(tmp);

    /* 5. O_WRONLY write then read-back */
    fd = open(tmp, O_CREAT | O_WRONLY, 0644);
    CHECK("open O_CREAT|O_WRONLY", fd >= 0);
    if (fd >= 0) {
        write(fd, "hello", 5);
        close(fd);
    }
    fd = open(tmp, O_RDONLY);
    CHECK("open for readback", fd >= 0);
    if (fd >= 0) {
        char buf[8] = {0};
        ssize_t n = read(fd, buf, sizeof(buf));
        CHECK("readback content", n == 5 && memcmp(buf, "hello", 5) == 0);
        close(fd);
    }
    unlink(tmp);

    /* 6. O_TRUNC truncates existing file */
    fd = open(tmp, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) { write(fd, "longdata", 8); close(fd); }
    fd = open(tmp, O_WRONLY | O_TRUNC);
    CHECK("open O_TRUNC", fd >= 0);
    if (fd >= 0) close(fd);
    fd = open(tmp, O_RDONLY);
    if (fd >= 0) {
        char buf[8];
        ssize_t n = read(fd, buf, sizeof(buf));
        CHECK("O_TRUNC file empty", n == 0);
        close(fd);
    }
    unlink(tmp);

    /* 7. O_APPEND appends */
    fd = open(tmp, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) { write(fd, "AB", 2); close(fd); }
    fd = open(tmp, O_WRONLY | O_APPEND);
    if (fd >= 0) { write(fd, "CD", 2); close(fd); }
    fd = open(tmp, O_RDONLY);
    if (fd >= 0) {
        char buf[8] = {0};
        ssize_t n = read(fd, buf, sizeof(buf));
        CHECK("O_APPEND appends", n == 4 && memcmp(buf, "ABCD", 4) == 0);
        close(fd);
    }
    unlink(tmp);

    /* 8. O_CLOEXEC sets FD_CLOEXEC */
    fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK("open O_CLOEXEC", fd >= 0);
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFD);
        CHECK("O_CLOEXEC → FD_CLOEXEC set", (flags & FD_CLOEXEC) != 0);
        close(fd);
    }

    /* 9. openat with AT_FDCWD */
    fd = openat(AT_FDCWD, "/dev/null", O_RDONLY);
    CHECK("openat AT_FDCWD", fd >= 0);
    if (fd >= 0) close(fd);

    /* 10. close bad fd → EBADF */
    errno = 0;
    int r = close(9999);
    CHECK_ERRNO("close bad fd → EBADF", r == -1, EBADF);

    /* 11. O_RDWR read+write */
    fd = open(tmp, O_CREAT | O_RDWR, 0644);
    CHECK("open O_RDWR", fd >= 0);
    if (fd >= 0) {
        write(fd, "XY", 2);
        lseek(fd, 0, SEEK_SET);
        char buf[4] = {0};
        ssize_t n = read(fd, buf, 2);
        CHECK("O_RDWR read after write", n == 2 && buf[0] == 'X' && buf[1] == 'Y');
        close(fd);
    }
    unlink(tmp);

    /* 12. open directory O_RDONLY succeeds */
    fd = open("/tmp", O_RDONLY | O_DIRECTORY);
    CHECK("open dir O_DIRECTORY", fd >= 0);
    if (fd >= 0) close(fd);

    LTP_SUMMARY("open");
}
