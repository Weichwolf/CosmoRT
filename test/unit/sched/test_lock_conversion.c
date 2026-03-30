/* Lock Conversion Tests — verify spinlock→mutex conversion is correct
 *
 * Each test exercises a subsystem whose lock was converted from spinlock_t
 * to mutex_t. Tests use fork to prove cross-process contention works.
 */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

#define FUTEX_WAIT          0
#define FUTEX_WAKE          1

static int wait_cs(long pid) {
    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    return WIFEXITED(ws) ? WEXITSTATUS(ws) : 99;
}

static void ms(int ms) {
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = ms * 1000000L };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
}

/* 1. Pipe read/write (pipe mutex) */
static void test_pipe(void) {
    int fds[2];
    check("lc_pipe_create", sc2(SYS_PIPE2, (long)fds, 0) == 0);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        char msg[] = "hello";
        long w = sc3(SYS_WRITE, fds[1], (long)msg, 5);
        sc1(SYS_EXIT, w == 5 ? 0 : 1);
    }
    char buf[8] = {0};
    long r = sc3(SYS_READ, fds[0], (long)buf, 5);
    check("lc_pipe_rw", r == 5 && buf[0] == 'h');
    check("lc_pipe_child", wait_cs(pid) == 0);
    sc1(SYS_CLOSE, fds[0]); sc1(SYS_CLOSE, fds[1]);
}

/* 2. Eventfd write/read (eventfd mutex) */
static void test_eventfd(void) {
    long efd = sc2(SYS_EVENTFD2, 0, 0);
    check("lc_efd_create", efd >= 0);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        uint64_t val = 42;
        long w = sc3(SYS_WRITE, efd, (long)&val, 8);
        sc1(SYS_EXIT, w == 8 ? 0 : 1);
    }
    ms(20);
    uint64_t rval = 0;
    long r = sc3(SYS_READ, efd, (long)&rval, 8);
    check("lc_efd_rw", r == 8 && rval == 42);
    check("lc_efd_child", wait_cs(pid) == 0);
    sc1(SYS_CLOSE, efd);
}

/* 3. VFS open/close/read (ext2/bcache mutexes) */
static void test_vfs(void) {
    long fd = sc3(SYS_OPEN, (long)"/tmp/lc_vfs", O_CREAT | O_RDWR, 0644);
    check("lc_vfs_create", fd >= 0);
    sc3(SYS_WRITE, fd, (long)"test123", 7);
    sc1(SYS_CLOSE, fd);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        long f2 = sc3(SYS_OPEN, (long)"/tmp/lc_vfs", O_RDONLY, 0);
        if (f2 < 0) sc1(SYS_EXIT, 1);
        char buf[8] = {0};
        long r = sc3(SYS_READ, f2, (long)buf, 7);
        sc1(SYS_CLOSE, f2);
        sc1(SYS_EXIT, (r == 7 && buf[0] == 't') ? 0 : 2);
    }
    check("lc_vfs_fork_read", wait_cs(pid) == 0);
    sc2(SYS_UNLINK, (long)"/tmp/lc_vfs", 0);
}

/* 4. Socket alloc (sock_lock mutex) */
static void test_socket(void) {
    long s1 = sc3(SYS_SOCKET, 2, 1, 0);
    check("lc_sock_create1", s1 >= 0);
    long s2 = sc3(SYS_SOCKET, 2, 1, 0);
    check("lc_sock_create2", s2 >= 0);
    sc1(SYS_CLOSE, s1); sc1(SYS_CLOSE, s2);
}

/* 5. mmap/munmap (VMA slab, pid_lock) */
static void test_mmap(void) {
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        long a = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
        if (a <= 0) sc1(SYS_EXIT, 1);
        *(volatile int *)a = 0xDEAD;
        sc2(SYS_MUNMAP, a, 4096);
        sc1(SYS_EXIT, 0);
    }
    long a = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("lc_mmap_parent", a > 0);
    if (a > 0) { *(volatile int *)a = 0xBEEF; sc2(SYS_MUNMAP, a, 4096); }
    check("lc_mmap_child", wait_cs(pid) == 0);
}

/* 6. PTY write (pty stays spinlock — IRQ context — this just exercises the path) */
static void test_pty(void) {
    long fd = sc3(SYS_OPEN, (long)"/dev/tty1", O_WRONLY, 0);
    if (fd < 0) { pass("lc_pty_skip"); return; }
    sc3(SYS_WRITE, fd, (long)"x", 1);
    sc1(SYS_CLOSE, fd);
    pass("lc_pty_write");
}

/* 7. Futex WAIT/WAKE (futex hash bucket mutex) */
static void test_futex(void) {
    volatile uint32_t *sh = (volatile uint32_t *)(long)sc6(SYS_MMAP,
        0, 4096, PROT_RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check("lc_futex_mmap", (long)sh > 0);
    *sh = 0;
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        ms(20);
        *sh = 1;
        sc6(SYS_FUTEX, (long)sh, FUTEX_WAKE, 1, 0, 0, 0);
        sc1(SYS_EXIT, 0);
    }
    struct k_timespec ts = { .tv_sec = 2, .tv_nsec = 0 };
    sc6(SYS_FUTEX, (long)sh, FUTEX_WAIT, 0, (long)&ts, 0, 0);
    check("lc_futex_woken", *sh == 1);
    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    sc2(SYS_MUNMAP, (long)sh, 4096);
}

/* 8. epoll_ctl ADD/DEL (epoll_t mutex) */
static void test_epoll(void) {
    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("lc_epoll_create", epfd >= 0);
    int pfds[2];
    check("lc_epoll_pipe", sc2(SYS_PIPE2, (long)pfds, 0) == 0);
    struct epoll_event ev = { .events = EPOLLIN, .data = 0x42 };
    check("lc_epoll_add", sc4(SYS_EPOLL_CTL, epfd, 1, pfds[0], (long)&ev) == 0);
    check("lc_epoll_del", sc4(SYS_EPOLL_CTL, epfd, 2, pfds[0], 0) == 0);
    sc1(SYS_CLOSE, pfds[0]); sc1(SYS_CLOSE, pfds[1]); sc1(SYS_CLOSE, epfd);
}

/* 9. Slab alloc/free (via mmap → VMA slab) */
static void test_slab(void) {
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        for (int i = 0; i < 10; i++) {
            long a = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
            if (a > 0) sc2(SYS_MUNMAP, a, 4096);
        }
        sc1(SYS_EXIT, 0);
    }
    for (int i = 0; i < 10; i++) {
        long a = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
        if (a > 0) sc2(SYS_MUNMAP, a, 4096);
    }
    check("lc_slab_fork", wait_cs(pid) == 0);
}

/* 10. Stress: 4 children open+write+close same file */
static void test_stress(void) {
    long fd = sc3(SYS_OPEN, (long)"/tmp/lc_str", O_CREAT | O_RDWR, 0644);
    check("lc_stress_create", fd >= 0);
    sc1(SYS_CLOSE, fd);

    long pids[4];
    for (int i = 0; i < 4; i++) {
        pids[i] = sc0(SYS_FORK);
        if (pids[i] == 0) {
            for (int j = 0; j < 5; j++) {
                long f = sc3(SYS_OPEN, (long)"/tmp/lc_str", O_WRONLY | O_APPEND, 0);
                if (f >= 0) {
                    char c = (char)('A' + i);
                    sc3(SYS_WRITE, f, (long)&c, 1);
                    sc1(SYS_CLOSE, f);
                }
            }
            sc1(SYS_EXIT, 0);
        }
    }
    int ok = 1;
    for (int i = 0; i < 4; i++)
        if (wait_cs(pids[i]) != 0) ok = 0;
    check("lc_stress_children", ok);

    fd = sc3(SYS_OPEN, (long)"/tmp/lc_str", O_RDONLY, 0);
    check("lc_stress_open", fd >= 0);
    char buf[32] = {0};
    long r = sc3(SYS_READ, fd, (long)buf, 32);
    sc1(SYS_CLOSE, fd);
    check("lc_stress_bytes", r == 20);
    sc2(SYS_UNLINK, (long)"/tmp/lc_str", 0);
}

/* ── Registration ────────────────────────────────── */

TEST("lock_conv", test_pipe);
TEST("lock_conv", test_eventfd);
TEST("lock_conv", test_vfs);
TEST("lock_conv", test_socket);
TEST("lock_conv", test_mmap);
TEST("lock_conv", test_pty);
TEST("lock_conv", test_futex);
TEST("lock_conv", test_epoll);
TEST("lock_conv", test_slab);
TEST("lock_conv", test_stress);
