#include "ktest.h"

/* inotify_event struct layout (Linux ABI) */
struct ino_event {
    int32_t  wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
    /* char name[] follows — variable length */
};

static int ino_strcmp(const char *a, const char *b) {
    while (*a && *b) { if (*a++ != *b++) return 1; }
    return *a != *b;
}

/* Helpers */
static long ino_init(int flags) {
    return sc1(SYS_INOTIFY_INIT1, flags);
}

static long ino_watch(int ifd, const char *path, long mask) {
    return sc3(SYS_INOTIFY_ADD_WATCH, ifd, (long)path, mask);
}

static long ino_rm(int ifd, int wd) {
    return sc2(SYS_INOTIFY_RM_WATCH, ifd, wd);
}

static void drain_events(int ifd) {
    char buf[512];
    while (sc3(SYS_READ, ifd, (long)buf, 512) > 0) {}
}

/* ── test_inotify_create ─────────────────────────── */

static void test_inotify_create(void) {
    puts("\n[inotify: create]\n");

    /* Setup: ensure /tmp exists */
    sc2(SYS_MKDIR, (long)"/tmp/ino_test", 0755);

    long ifd = ino_init(O_NONBLOCK | O_CLOEXEC);
    check("inotify_init1 >= 0", ifd >= 0);
    if (ifd < 0) return;

    long wd = ino_watch(ifd, "/tmp/ino_test", IN_CREATE);
    check("add_watch >= 1", wd >= 1);

    /* Create a file in watched dir */
    long fd = sc3(SYS_OPEN, (long)"/tmp/ino_test/newfile", O_CREAT | O_WRONLY, 0644);
    check("open(O_CREAT) >= 0", fd >= 0);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    /* Read event */
    char buf[256];
    long n = sc3(SYS_READ, ifd, (long)buf, 256);
    check("read > 0", n > 0);

    if (n > 0) {
        struct ino_event *ev = (struct ino_event *)buf;
        check_val("wd matches", ev->wd, wd);
        check("mask == IN_CREATE", (ev->mask & IN_CREATE) != 0);
        check("name present", ev->len > 0);
        if (ev->len > 0) {
            char *name = (char *)ev + 16;
            check("name == newfile", ino_strcmp(name, "newfile") == 0);
        }
    }

    /* Cleanup */
    sc1(SYS_CLOSE, ifd);
    sc1(SYS_UNLINK, (long)"/tmp/ino_test/newfile");
    sc1(SYS_RMDIR, (long)"/tmp/ino_test");
}

TEST("inotify_create", test_inotify_create);

/* ── test_inotify_modify ─────────────────────────── */

static void test_inotify_modify(void) {
    puts("\n[inotify: modify]\n");

    sc2(SYS_MKDIR, (long)"/tmp/ino_mod", 0755);

    /* Create file first */
    long fd = sc3(SYS_OPEN, (long)"/tmp/ino_mod/file", O_CREAT | O_WRONLY, 0644);
    check("create file >= 0", fd >= 0);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    long ifd = ino_init(O_NONBLOCK);
    check("inotify_init >= 0", ifd >= 0);
    if (ifd < 0) return;

    /* Watch the directory for IN_MODIFY */
    long wd = ino_watch(ifd, "/tmp/ino_mod", IN_MODIFY);
    check("add_watch >= 1", wd >= 1);

    /* Write to the file */
    fd = sc3(SYS_OPEN, (long)"/tmp/ino_mod/file", O_WRONLY, 0);
    check("open(WRONLY) >= 0", fd >= 0);
    if (fd >= 0) {
        sc3(SYS_WRITE, fd, (long)"hello", 5);
        sc1(SYS_CLOSE, fd);
    }

    /* Read event */
    char buf[256];
    long n = sc3(SYS_READ, ifd, (long)buf, 256);
    check("read > 0", n > 0);

    if (n > 0) {
        struct ino_event *ev = (struct ino_event *)buf;
        check("mask has IN_MODIFY", (ev->mask & IN_MODIFY) != 0);
    }

    sc1(SYS_CLOSE, ifd);
    sc1(SYS_UNLINK, (long)"/tmp/ino_mod/file");
    sc1(SYS_RMDIR, (long)"/tmp/ino_mod");
}

TEST("inotify_modify", test_inotify_modify);

/* ── test_inotify_delete ─────────────────────────── */

static void test_inotify_delete(void) {
    puts("\n[inotify: delete]\n");

    sc2(SYS_MKDIR, (long)"/tmp/ino_del", 0755);

    /* Create file */
    long fd = sc3(SYS_OPEN, (long)"/tmp/ino_del/victim", O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    long ifd = ino_init(O_NONBLOCK);
    check("inotify_init >= 0", ifd >= 0);
    if (ifd < 0) return;

    long wd = ino_watch(ifd, "/tmp/ino_del", IN_DELETE);
    check("add_watch >= 1", wd >= 1);

    /* Delete the file */
    long r = sc1(SYS_UNLINK, (long)"/tmp/ino_del/victim");
    check_val("unlink == 0", r, 0);

    /* Read event */
    char buf[256];
    long n = sc3(SYS_READ, ifd, (long)buf, 256);
    check("read > 0", n > 0);

    if (n > 0) {
        struct ino_event *ev = (struct ino_event *)buf;
        check("mask == IN_DELETE", (ev->mask & IN_DELETE) != 0);
        if (ev->len > 0) {
            char *name = (char *)ev + 16;
            check("name == victim", ino_strcmp(name, "victim") == 0);
        }
    }

    sc1(SYS_CLOSE, ifd);
    sc1(SYS_RMDIR, (long)"/tmp/ino_del");
}

TEST("inotify_delete", test_inotify_delete);

/* ── test_inotify_move ───────────────────────────── */

static void test_inotify_move(void) {
    puts("\n[inotify: move]\n");

    sc2(SYS_MKDIR, (long)"/tmp/ino_mv", 0755);

    long fd = sc3(SYS_OPEN, (long)"/tmp/ino_mv/src", O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    long ifd = ino_init(O_NONBLOCK);
    check("inotify_init >= 0", ifd >= 0);
    if (ifd < 0) return;

    long wd = ino_watch(ifd, "/tmp/ino_mv", IN_MOVED_FROM | IN_MOVED_TO);
    check("add_watch >= 1", wd >= 1);

    /* Rename */
    sc2(SYS_RENAME, (long)"/tmp/ino_mv/src", (long)"/tmp/ino_mv/dst");

    /* Read events — expect MOVED_FROM then MOVED_TO */
    char buf[512];
    long n = sc3(SYS_READ, ifd, (long)buf, 512);
    check("read > 0", n > 0);

    int got_from = 0, got_to = 0;
    long off = 0;
    while (off + 16 <= n) {
        struct ino_event *ev = (struct ino_event *)(buf + off);
        if (ev->mask & IN_MOVED_FROM) got_from = 1;
        if (ev->mask & IN_MOVED_TO)   got_to = 1;
        off += 16 + (long)ev->len;
    }
    check("got IN_MOVED_FROM", got_from);
    check("got IN_MOVED_TO", got_to);

    sc1(SYS_CLOSE, ifd);
    sc1(SYS_UNLINK, (long)"/tmp/ino_mv/dst");
    sc1(SYS_RMDIR, (long)"/tmp/ino_mv");
}

TEST("inotify_move", test_inotify_move);

/* ── test_inotify_close ──────────────────────────── */

static void test_inotify_close(void) {
    puts("\n[inotify: close_write]\n");

    sc2(SYS_MKDIR, (long)"/tmp/ino_cl", 0755);

    long ifd = ino_init(O_NONBLOCK);
    check("inotify_init >= 0", ifd >= 0);
    if (ifd < 0) return;

    long wd = ino_watch(ifd, "/tmp/ino_cl", IN_CLOSE_WRITE | IN_CREATE);
    check("add_watch >= 1", wd >= 1);

    /* Create + write + close */
    long fd = sc3(SYS_OPEN, (long)"/tmp/ino_cl/wr", O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) {
        sc3(SYS_WRITE, fd, (long)"x", 1);
        sc1(SYS_CLOSE, fd);
    }

    /* Read events — expect IN_CREATE + IN_CLOSE_WRITE */
    char buf[512];
    long n = sc3(SYS_READ, ifd, (long)buf, 512);
    check("read > 0", n > 0);

    int got_close = 0;
    long off = 0;
    while (off + 16 <= n) {
        struct ino_event *ev = (struct ino_event *)(buf + off);
        if (ev->mask & IN_CLOSE_WRITE) got_close = 1;
        off += 16 + (long)ev->len;
    }
    check("got IN_CLOSE_WRITE", got_close);

    sc1(SYS_CLOSE, ifd);
    sc1(SYS_UNLINK, (long)"/tmp/ino_cl/wr");
    sc1(SYS_RMDIR, (long)"/tmp/ino_cl");
}

TEST("inotify_close", test_inotify_close);

/* ── test_inotify_mask ───────────────────────────── */

static void test_inotify_mask(void) {
    puts("\n[inotify: mask filter]\n");

    sc2(SYS_MKDIR, (long)"/tmp/ino_mask", 0755);

    long ifd = ino_init(O_NONBLOCK);
    check("inotify_init >= 0", ifd >= 0);
    if (ifd < 0) return;

    /* Watch only IN_DELETE — should NOT get IN_CREATE */
    long wd = ino_watch(ifd, "/tmp/ino_mask", IN_DELETE);
    check("add_watch >= 1", wd >= 1);

    /* Create a file */
    long fd = sc3(SYS_OPEN, (long)"/tmp/ino_mask/file", O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    /* Should have no events (we didn't watch IN_CREATE) */
    char buf[256];
    long n = sc3(SYS_READ, ifd, (long)buf, 256);
    /* May get IN_CLOSE_WRITE too if it leaks — but we only watch IN_DELETE */
    check("no events for unwatched mask", n <= 0);

    /* Now delete — should get IN_DELETE */
    sc1(SYS_UNLINK, (long)"/tmp/ino_mask/file");
    n = sc3(SYS_READ, ifd, (long)buf, 256);
    check("got event after delete", n > 0);
    if (n > 0) {
        struct ino_event *ev = (struct ino_event *)buf;
        check("event is IN_DELETE", (ev->mask & IN_DELETE) != 0);
    }

    sc1(SYS_CLOSE, ifd);
    sc1(SYS_RMDIR, (long)"/tmp/ino_mask");
}

TEST("inotify_mask", test_inotify_mask);

/* ── test_inotify_multiple ───────────────────────── */

static void test_inotify_multiple(void) {
    puts("\n[inotify: multiple watches]\n");

    sc2(SYS_MKDIR, (long)"/tmp/ino_m1", 0755);
    sc2(SYS_MKDIR, (long)"/tmp/ino_m2", 0755);

    long ifd = ino_init(O_NONBLOCK);
    check("inotify_init >= 0", ifd >= 0);
    if (ifd < 0) return;

    long wd1 = ino_watch(ifd, "/tmp/ino_m1", IN_CREATE);
    long wd2 = ino_watch(ifd, "/tmp/ino_m2", IN_CREATE);
    check("wd1 >= 1", wd1 >= 1);
    check("wd2 >= 1", wd2 >= 1);
    check("wd1 != wd2", wd1 != wd2);

    /* Create in dir2 */
    long fd = sc3(SYS_OPEN, (long)"/tmp/ino_m2/file", O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    /* Event should be for wd2, not wd1 */
    char buf[256];
    long n = sc3(SYS_READ, ifd, (long)buf, 256);
    check("read > 0", n > 0);
    if (n > 0) {
        struct ino_event *ev = (struct ino_event *)buf;
        check_val("event wd == wd2", ev->wd, wd2);
    }

    sc1(SYS_CLOSE, ifd);
    sc1(SYS_UNLINK, (long)"/tmp/ino_m2/file");
    sc1(SYS_RMDIR, (long)"/tmp/ino_m1");
    sc1(SYS_RMDIR, (long)"/tmp/ino_m2");
}

TEST("inotify_multiple", test_inotify_multiple);

/* ── test_inotify_poll ───────────────────────────── */

static void test_inotify_poll(void) {
    puts("\n[inotify: poll/epoll]\n");

    sc2(SYS_MKDIR, (long)"/tmp/ino_poll", 0755);

    long ifd = ino_init(O_NONBLOCK);
    check("inotify_init >= 0", ifd >= 0);
    if (ifd < 0) return;

    long wd = ino_watch(ifd, "/tmp/ino_poll", IN_CREATE);
    check("add_watch >= 1", wd >= 1);

    /* Create epoll and add inotify fd */
    long efd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1 >= 0", efd >= 0);
    if (efd < 0) { sc1(SYS_CLOSE, ifd); return; }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data = (uint64_t)42;
    long r = sc4(SYS_EPOLL_CTL, efd, EPOLL_CTL_ADD, ifd, (long)&ev);
    check_val("epoll_ctl(ADD) == 0", r, 0);

    /* No events yet — epoll_wait with 0 timeout should return 0 */
    struct epoll_event out;
    r = sc4(SYS_EPOLL_WAIT, efd, (long)&out, 1, 0);
    check_val("epoll_wait(0) = 0 (no events)", r, 0);

    /* Now trigger an event */
    long fd = sc3(SYS_OPEN, (long)"/tmp/ino_poll/file", O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    /* epoll_wait should now return 1 */
    r = sc4(SYS_EPOLL_WAIT, efd, (long)&out, 1, 0);
    check("epoll_wait = 1 (events ready)", r >= 1);
    if (r >= 1) {
        check("EPOLLIN set", (out.events & EPOLLIN) != 0);
        check_val("data == 42", (long)out.data, 42);
    }

    sc1(SYS_CLOSE, efd);
    sc1(SYS_CLOSE, ifd);
    sc1(SYS_UNLINK, (long)"/tmp/ino_poll/file");
    sc1(SYS_RMDIR, (long)"/tmp/ino_poll");
}

TEST("inotify_poll", test_inotify_poll);

/* ── test_inotify_nonblock ───────────────────────── */

static void test_inotify_nonblock(void) {
    puts("\n[inotify: nonblock]\n");

    long ifd = ino_init(O_NONBLOCK);
    check("inotify_init >= 0", ifd >= 0);
    if (ifd < 0) return;

    /* Read with no events should return EAGAIN */
    char buf[128];
    long r = sc3(SYS_READ, ifd, (long)buf, 128);
    check_val("read empty = -EAGAIN", r, -EAGAIN);

    sc1(SYS_CLOSE, ifd);
}

TEST("inotify_nonblock", test_inotify_nonblock);

/* ── test_inotify_rm_watch ───────────────────────── */

static void test_inotify_rm_watch(void) {
    puts("\n[inotify: rm_watch]\n");

    sc2(SYS_MKDIR, (long)"/tmp/ino_rm", 0755);

    long ifd = ino_init(O_NONBLOCK);
    check("inotify_init >= 0", ifd >= 0);
    if (ifd < 0) return;

    long wd = ino_watch(ifd, "/tmp/ino_rm", IN_CREATE);
    check("add_watch >= 1", wd >= 1);

    /* Remove the watch */
    long r = ino_rm(ifd, (int)wd);
    check_val("rm_watch == 0", r, 0);

    /* Drain any IN_IGNORED event */
    drain_events(ifd);

    /* Create file — no events should be generated */
    long fd = sc3(SYS_OPEN, (long)"/tmp/ino_rm/file", O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    char buf[256];
    long n = sc3(SYS_READ, ifd, (long)buf, 256);
    check("no events after rm_watch", n <= 0);

    sc1(SYS_CLOSE, ifd);
    sc1(SYS_UNLINK, (long)"/tmp/ino_rm/file");
    sc1(SYS_RMDIR, (long)"/tmp/ino_rm");
}

TEST("inotify_rm_watch", test_inotify_rm_watch);

/* ── test_inotify_overflow ───────────────────────── */

static void test_inotify_overflow(void) {
    puts("\n[inotify: overflow]\n");

    sc2(SYS_MKDIR, (long)"/tmp/ino_ovf", 0755);

    long ifd = ino_init(O_NONBLOCK);
    check("inotify_init >= 0", ifd >= 0);
    if (ifd < 0) return;

    long wd = ino_watch(ifd, "/tmp/ino_ovf", IN_CREATE);
    check("add_watch >= 1", wd >= 1);

    /* Generate many events to overflow the ring buffer.
     * Each event is ~16+12 = 28 bytes. Ring is 2048 bytes.
     * ~73 events should fill it. Create 100 files. */
    char path[64];
    for (int i = 0; i < 100; i++) {
        /* Build path like /tmp/ino_ovf/f000 */
        path[0] = '/'; path[1] = 't'; path[2] = 'm'; path[3] = 'p';
        path[4] = '/'; path[5] = 'i'; path[6] = 'n'; path[7] = 'o';
        path[8] = '_'; path[9] = 'o'; path[10] = 'v'; path[11] = 'f';
        path[12] = '/'; path[13] = 'f';
        path[14] = '0' + (char)(i / 100 % 10);
        path[15] = '0' + (char)(i / 10 % 10);
        path[16] = '0' + (char)(i % 10);
        path[17] = 0;
        long fd = sc3(SYS_OPEN, (long)path, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) sc1(SYS_CLOSE, fd);
    }

    /* Drain all events */
    char buf[4096];
    long total = 0;
    int got_overflow = 0;
    for (int tries = 0; tries < 20; tries++) {
        long n = sc3(SYS_READ, ifd, (long)buf, 4096);
        if (n <= 0) break;
        /* Scan for IN_Q_OVERFLOW */
        long off = 0;
        while (off + 16 <= n) {
            struct ino_event *ev = (struct ino_event *)(buf + off);
            if (ev->mask & IN_Q_OVERFLOW) got_overflow = 1;
            off += 16 + (long)ev->len;
        }
        total += n;
    }
    check("got events or overflow", total > 0 || got_overflow);
    check("overflow detected", got_overflow);

    /* Cleanup */
    sc1(SYS_CLOSE, ifd);
    for (int i = 0; i < 100; i++) {
        path[0] = '/'; path[1] = 't'; path[2] = 'm'; path[3] = 'p';
        path[4] = '/'; path[5] = 'i'; path[6] = 'n'; path[7] = 'o';
        path[8] = '_'; path[9] = 'o'; path[10] = 'v'; path[11] = 'f';
        path[12] = '/'; path[13] = 'f';
        path[14] = '0' + (char)(i / 100 % 10);
        path[15] = '0' + (char)(i / 10 % 10);
        path[16] = '0' + (char)(i % 10);
        path[17] = 0;
        sc1(SYS_UNLINK, (long)path);
    }
    sc1(SYS_RMDIR, (long)"/tmp/ino_ovf");
}

TEST("inotify_overflow", test_inotify_overflow);

/* ── test_inotify_attrib ─────────────────────────── */

static void test_inotify_attrib(void) {
    puts("\n[inotify: attrib]\n");

    sc2(SYS_MKDIR, (long)"/tmp/ino_attr", 0755);

    long fd = sc3(SYS_OPEN, (long)"/tmp/ino_attr/file", O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    long ifd = ino_init(O_NONBLOCK);
    check("inotify_init >= 0", ifd >= 0);
    if (ifd < 0) return;

    /* Watch the file directly for IN_ATTRIB */
    long wd = ino_watch(ifd, "/tmp/ino_attr/file", IN_ATTRIB);
    check("add_watch >= 1", wd >= 1);

    /* chmod the file */
    sc2(SYS_CHMOD, (long)"/tmp/ino_attr/file", 0600);

    /* Read event */
    char buf[128];
    long n = sc3(SYS_READ, ifd, (long)buf, 128);
    check("read > 0", n > 0);
    if (n > 0) {
        struct ino_event *ev = (struct ino_event *)buf;
        check("mask has IN_ATTRIB", (ev->mask & IN_ATTRIB) != 0);
    }

    sc1(SYS_CLOSE, ifd);
    sc1(SYS_UNLINK, (long)"/tmp/ino_attr/file");
    sc1(SYS_RMDIR, (long)"/tmp/ino_attr");
}

TEST("inotify_attrib", test_inotify_attrib);
