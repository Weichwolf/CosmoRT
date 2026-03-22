#include "ktest.h"

static void test_vfs(void) {
    puts("\n[VFS]\n");

    /* Create and write a file */
    long fd = sc3(SYS_open, (long)"/test.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    check("open /test.txt for write", fd >= 0);
    if (fd >= 0) {
        const char *data = "Hello from ktest!";
        long w = sc3(SYS_write, fd, (long)data, 17);
        check_val("write 17 bytes", w, 17);
        sc1(SYS_close, fd);
    }

    /* Read it back */
    fd = sc3(SYS_open, (long)"/test.txt", O_RDONLY, 0);
    check("open /test.txt for read", fd >= 0);
    if (fd >= 0) {
        char rbuf[32] = {0};
        long r = sc3(SYS_read, fd, (long)rbuf, 17);
        check_val("read 17 bytes", r, 17);
        check("read data matches",
              rbuf[0]=='H' && rbuf[1]=='e' && rbuf[2]=='l' && rbuf[3]=='l' && rbuf[4]=='o');
        sc1(SYS_close, fd);
    }

    /* getcwd */
    char cwd[128] = {0};
    long r = sc2(SYS_getcwd, (long)cwd, 128);
    check("getcwd succeeds", r > 0);
    check("cwd is /", cwd[0] == '/' && (cwd[1] == 0 || cwd[1] == '\n'));

    /* fstat on closed fd */
    struct {
        uint64_t dev, ino, nlink;
        uint32_t mode, uid, gid, pad;
        uint64_t rdev;
        int64_t size, blksize, blocks;
        int64_t atime_s, atime_ns, mtime_s, mtime_ns, ctime_s, ctime_ns;
        int64_t unused[3];
    } st;
    r = sc2(SYS_fstat, fd, (long)&st);
    /* fd is closed, should fail */
    check("fstat on closed fd fails", r < 0);
}

TEST("vfs", test_vfs);
