/* LTP dup01-07/dup201-207/dup3_01/dup3_02 — ported to ktest */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)

/* ── dup01: basic dup returns new fd referencing same inode ── */

static void test_dup01(void) {
    puts("\n[ltp/dup01]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/dup01_file", O_RDWR | O_CREAT, 0700);
    check("open", fd >= 0);
    if (fd < 0) return;

    long nfd = sc1(SYS_DUP, fd);
    check("dup returns valid fd", nfd >= 0);
    if (nfd < 0) { sc1(SYS_CLOSE, fd); return; }

    check("dup returns different fd", nfd != fd);

    /* Both fds should stat to same inode */
    struct k_stat st1, st2;
    long r1 = sc2(SYS_FSTAT, fd, (long)&st1);
    long r2 = sc2(SYS_FSTAT, nfd, (long)&st2);
    check_val("fstat orig", r1, 0);
    check_val("fstat dup", r2, 0);
    if (r1 == 0 && r2 == 0)
        check_val("same inode", (long)st1.st_ino, (long)st2.st_ino);

    sc1(SYS_CLOSE, nfd);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/dup01_file");
}

/* ── dup02: EBADF for invalid fd ── */

static void test_dup02(void) {
    puts("\n[ltp/dup02]\n");

    long r = sc1(SYS_DUP, -1);
    check_val("dup(-1) EBADF", r, -EBADF);

    r = sc1(SYS_DUP, 1500);
    check_val("dup(1500) EBADF", r, -EBADF);
}

/* ── dup03: EMFILE when fd table full ── */

static void test_dup03(void) {
    puts("\n[ltp/dup03]\n");

    /* Open a file and dup until EMFILE */
    long base = sc3(SYS_OPEN, (long)"/tmp/dup03_file", O_RDWR | O_CREAT, 0700);
    check("open base", base >= 0);
    if (base < 0) return;

    /* Fill up to limit */
    int count = 0;
    long last_good = -1;
    for (;;) {
        long d = sc1(SYS_DUP, base);
        if (d < 0) {
            check_val("dup EMFILE", d, -EMFILE);
            break;
        }
        last_good = d;
        count++;
        if (count > 4096) {
            /* Safety stop */
            fail("dup EMFILE", "did not hit limit in 4096 dups");
            break;
        }
    }

    /* Close all duped fds */
    if (last_good >= 0) {
        for (long i = base + 1; i <= last_good; i++)
            sc1(SYS_CLOSE, i);
    }
    sc1(SYS_CLOSE, base);
    sc1(SYS_UNLINK, (long)"/tmp/dup03_file");
}

/* ── dup04: dup of pipe fd ── */

static void test_dup04(void) {
    puts("\n[ltp/dup04]\n");

    long pipefd[2];
    long r = sc1(SYS_PIPE, (long)pipefd);
    check_val("pipe", r, 0);
    if (r != 0) return;

    long d0 = sc1(SYS_DUP, pipefd[0]);
    check("dup pipe read end", d0 >= 0);
    if (d0 >= 0) sc1(SYS_CLOSE, d0);

    long d1 = sc1(SYS_DUP, pipefd[1]);
    check("dup pipe write end", d1 >= 0);
    if (d1 >= 0) sc1(SYS_CLOSE, d1);

    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

/* ── dup07: dup preserves file mode ── */

static void test_dup07(void) {
    puts("\n[ltp/dup07]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/dup07_file", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    long nfd = sc1(SYS_DUP, fd);
    check("dup", nfd >= 0);
    if (nfd < 0) { sc1(SYS_CLOSE, fd); return; }

    struct k_stat st1, st2;
    sc2(SYS_FSTAT, fd, (long)&st1);
    sc2(SYS_FSTAT, nfd, (long)&st2);
    check_val("same mode", (long)(st1.st_mode & 0xFFF), (long)(st2.st_mode & 0xFFF));

    sc1(SYS_CLOSE, nfd);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/dup07_file");
}

/* ── dup201: dup2 EBADF for bad oldfd/newfd ── */

static void test_dup201(void) {
    puts("\n[ltp/dup201]\n");

    long r = sc2(SYS_DUP2, -1, 5);
    check_val("dup2(-1,5) EBADF", r, -EBADF);

    /* maxfd = use a very large value */
    r = sc2(SYS_DUP2, 9999, 5);
    check_val("dup2(9999,5) EBADF", r, -EBADF);

    /* valid oldfd, bad newfd */
    r = sc2(SYS_DUP2, 1, -1);
    check_val("dup2(1,-1) EBADF", r, -EBADF);
}

/* ── dup203: dup2 basic functionality — dupes over open and closed fd ── */

static void test_dup203_open(void) {
    puts("\n[ltp/dup203-open]\n");

    /* Create file, write data, reopen for reading */
    long fd0 = sc3(SYS_OPEN, (long)"/tmp/dup203_f0", O_RDWR | O_CREAT | O_TRUNC, 0666);
    check("create f0", fd0 >= 0);
    if (fd0 < 0) return;
    sc3(SYS_WRITE, fd0, (long)"hello", 5);
    sc1(SYS_CLOSE, fd0);

    fd0 = sc3(SYS_OPEN, (long)"/tmp/dup203_f0", O_RDONLY, 0);
    check("reopen f0", fd0 >= 0);

    /* Create another open fd */
    long fd1 = sc3(SYS_OPEN, (long)"/tmp/dup203_f1", O_RDWR | O_CREAT | O_TRUNC, 0666);
    check("create f1", fd1 >= 0);

    /* dup2 fd0 over fd1 */
    long r = sc2(SYS_DUP2, fd0, fd1);
    check_val("dup2 returns newfd", r, fd1);

    /* Read through fd1 should give same data as fd0 */
    char buf[8] = {0};
    long n = sc3(SYS_READ, fd1, (long)buf, 5);
    check_val("read 5 bytes", n, 5);
    check("data matches", buf[0] == 'h' && buf[4] == 'o');

    /* FD_CLOEXEC should be cleared on the new fd */
    long flags = sc2(SYS_FCNTL, fd1, F_GETFD);
    check("FD_CLOEXEC clear", (flags & FD_CLOEXEC) == 0);

    sc1(SYS_CLOSE, fd0);
    sc1(SYS_CLOSE, fd1);
    sc1(SYS_UNLINK, (long)"/tmp/dup203_f0");
    sc1(SYS_UNLINK, (long)"/tmp/dup203_f1");
}

/* ── dup204: dup2 on pipe — same inode ── */

static void test_dup204(void) {
    puts("\n[ltp/dup204]\n");

    long pipefd[2];
    long r = sc1(SYS_PIPE, (long)pipefd);
    check_val("pipe", r, 0);
    if (r != 0) return;

    int nfd0 = 10, nfd1 = 20;
    r = sc2(SYS_DUP2, pipefd[0], nfd0);
    check_val("dup2 pipe[0]->10", r, nfd0);

    r = sc2(SYS_DUP2, pipefd[1], nfd1);
    check_val("dup2 pipe[1]->20", r, nfd1);

    struct k_stat st_old, st_new;
    sc2(SYS_FSTAT, pipefd[0], (long)&st_old);
    sc2(SYS_FSTAT, nfd0, (long)&st_new);
    check_val("pipe[0] same inode", (long)st_old.st_ino, (long)st_new.st_ino);

    sc1(SYS_CLOSE, nfd0);
    sc1(SYS_CLOSE, nfd1);
    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

/* ── dup206: dup2 oldfd==newfd returns newfd ── */

static void test_dup206(void) {
    puts("\n[ltp/dup206]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/dup206_file", O_RDWR | O_CREAT, 0666);
    check("open", fd >= 0);
    if (fd < 0) return;

    long r = sc2(SYS_DUP2, fd, fd);
    check_val("dup2(fd,fd) returns fd", r, fd);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/dup206_file");
}

/* ── dup207: dup2 shares file offset ── */

static void test_dup207(void) {
    puts("\n[ltp/dup207]\n");

    long ofd = sc3(SYS_OPEN, (long)"/tmp/dup207_file", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check("open", ofd >= 0);
    if (ofd < 0) return;

    sc3(SYS_WRITE, ofd, (long)"abcdefg", 7);

    /* Seek to offset 1, then dup2 */
    sc3(SYS_LSEEK, ofd, 1, SEEK_SET);

    int nfd = 10;
    long r = sc2(SYS_DUP2, ofd, nfd);
    check_val("dup2", r, nfd);

    /* Read from nfd should start at offset 1 */
    char buf[8] = {0};
    long n = sc3(SYS_READ, nfd, (long)buf, 6);
    check_val("read 6 bytes", n, 6);
    check("offset shared", buf[0] == 'b' && buf[5] == 'g');

    /* Seek on ofd after dup2, read from nfd */
    sc3(SYS_LSEEK, ofd, 2, SEEK_SET);
    char buf2[8] = {0};
    n = sc3(SYS_READ, nfd, (long)buf2, 5);
    check_val("read 5 bytes after seek", n, 5);
    check("offset shared after seek", buf2[0] == 'c');

    sc1(SYS_CLOSE, nfd);
    sc1(SYS_CLOSE, ofd);
    sc1(SYS_UNLINK, (long)"/tmp/dup207_file");
}

/* ── dup3_01: O_CLOEXEC flag ── */

static void test_dup3_01_nocloexec(void) {
    puts("\n[ltp/dup3_01-nocloexec]\n");

    long r = sc3(SYS_DUP3, 1, 50, 0);
    check("dup3 no flags", r >= 0);
    if (r < 0) return;

    long flags = sc2(SYS_FCNTL, r, F_GETFD);
    check("no FD_CLOEXEC", (flags & FD_CLOEXEC) == 0);
    sc1(SYS_CLOSE, r);
}

static void test_dup3_01_cloexec(void) {
    puts("\n[ltp/dup3_01-cloexec]\n");

    long r = sc3(SYS_DUP3, 1, 51, O_CLOEXEC);
    check("dup3 O_CLOEXEC", r >= 0);
    if (r < 0) return;

    long flags = sc2(SYS_FCNTL, r, F_GETFD);
    check("FD_CLOEXEC set", (flags & FD_CLOEXEC) != 0);
    sc1(SYS_CLOSE, r);
}

/* ── dup3_02: EINVAL when oldfd==newfd ── */

static void test_dup3_02_same_fd(void) {
    puts("\n[ltp/dup3_02-same-fd]\n");

    /* oldfd == newfd with O_CLOEXEC — EINVAL */
    long r = sc3(SYS_DUP3, 1, 1, O_CLOEXEC);
    check_val("dup3 same fd EINVAL", r, -EINVAL);

    /* oldfd == newfd with flags=0 — also EINVAL */
    r = sc3(SYS_DUP3, 1, 1, 0);
    check_val("dup3 same fd flags=0 EINVAL", r, -EINVAL);
}

/* ── dup3_02: EINVAL for invalid flags ── */

static void test_dup3_02_invalid_flags(void) {
    puts("\n[ltp/dup3_02-invalid-flags]\n");

    long r = sc3(SYS_DUP3, 1, 52, -1);
    check_val("dup3 invalid flags EINVAL", r, -EINVAL);
}

TEST("ltp/dup01",            test_dup01);
TEST("ltp/dup02",            test_dup02);
TEST("ltp/dup03",            test_dup03);
TEST("ltp/dup04",            test_dup04);
TEST("ltp/dup07",            test_dup07);
TEST("ltp/dup201",           test_dup201);
TEST("ltp/dup203-open",      test_dup203_open);
TEST("ltp/dup204",           test_dup204);
TEST("ltp/dup206",           test_dup206);
TEST("ltp/dup207",           test_dup207);
TEST("ltp/dup3_01-nocloexec", test_dup3_01_nocloexec);
TEST("ltp/dup3_01-cloexec",  test_dup3_01_cloexec);
TEST("ltp/dup3_02-same-fd",  test_dup3_02_same_fd);
TEST("ltp/dup3_02-invalid-flags", test_dup3_02_invalid_flags);
