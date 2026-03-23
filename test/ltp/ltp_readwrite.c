/* LTP: read/write/pread/pwrite/readv/writev */
#define _GNU_SOURCE
#include "ltp.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/uio.h>

int main(void) {
    printf("=== LTP: readwrite ===\n");
    const char *tmp = "/tmp/ltp_rw_test";

    /* 1. write to file, read back */
    int fd = open(tmp, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK("open for rw", fd >= 0);
    ssize_t n = write(fd, "hello world", 11);
    CHECK("write 11 bytes", n == 11);
    lseek(fd, 0, SEEK_SET);
    char buf[32] = {0};
    n = read(fd, buf, sizeof(buf));
    CHECK("read back 11 bytes", n == 11 && memcmp(buf, "hello world", 11) == 0);
    close(fd);

    /* 2. read from /dev/null → 0 */
    fd = open("/dev/null", O_RDONLY);
    n = read(fd, buf, sizeof(buf));
    CHECK("read /dev/null → 0", n == 0);
    close(fd);

    /* 3. write to /dev/null → count */
    fd = open("/dev/null", O_WRONLY);
    n = write(fd, "discard", 7);
    CHECK("write /dev/null → 7", n == 7);
    close(fd);

    /* 4. read bad fd → EBADF */
    errno = 0;
    n = read(9999, buf, 1);
    CHECK_ERRNO("read bad fd → EBADF", n == -1, EBADF);

    /* 5. write bad fd → EBADF */
    errno = 0;
    n = write(9999, buf, 1);
    CHECK_ERRNO("write bad fd → EBADF", n == -1, EBADF);

    /* 6. pwrite + pread */
    fd = open(tmp, O_CREAT | O_RDWR | O_TRUNC, 0644);
    n = pwrite(fd, "ABCDEF", 6, 10);
    CHECK("pwrite 6 at offset 10", n == 6);
    memset(buf, 0, sizeof(buf));
    n = pread(fd, buf, 6, 10);
    CHECK("pread 6 at offset 10", n == 6 && memcmp(buf, "ABCDEF", 6) == 0);
    /* file position unchanged after pread/pwrite */
    off_t pos = lseek(fd, 0, SEEK_CUR);
    CHECK("pwrite/pread preserve offset", pos == 0);
    close(fd);

    /* 7. writev */
    fd = open(tmp, O_CREAT | O_RDWR | O_TRUNC, 0644);
    struct iovec wv[2] = {
        { .iov_base = (void *)"foo", .iov_len = 3 },
        { .iov_base = (void *)"bar", .iov_len = 3 },
    };
    n = writev(fd, wv, 2);
    CHECK("writev 2 iov → 6", n == 6);

    /* 8. readv */
    lseek(fd, 0, SEEK_SET);
    char b1[3] = {0}, b2[3] = {0};
    struct iovec rv[2] = {
        { .iov_base = b1, .iov_len = 3 },
        { .iov_base = b2, .iov_len = 3 },
    };
    n = readv(fd, rv, 2);
    CHECK("readv 2 iov → 6", n == 6 && memcmp(b1, "foo", 3) == 0 && memcmp(b2, "bar", 3) == 0);
    close(fd);

    /* 9. read 0 bytes → 0 */
    fd = open(tmp, O_RDONLY);
    n = read(fd, buf, 0);
    CHECK("read 0 bytes → 0", n == 0);
    close(fd);

    /* 10. write 0 bytes → 0 */
    fd = open(tmp, O_WRONLY);
    n = write(fd, buf, 0);
    CHECK("write 0 bytes → 0", n == 0);
    close(fd);

    /* 11. large sequential write + read */
    fd = open(tmp, O_CREAT | O_RDWR | O_TRUNC, 0644);
    char big[4096];
    memset(big, 'X', sizeof(big));
    n = write(fd, big, sizeof(big));
    CHECK("write 4096 bytes", n == 4096);
    lseek(fd, 0, SEEK_SET);
    char big2[4096];
    n = read(fd, big2, sizeof(big2));
    CHECK("read 4096 bytes", n == 4096 && memcmp(big, big2, 4096) == 0);
    close(fd);

    unlink(tmp);
    LTP_SUMMARY("readwrite");
}
