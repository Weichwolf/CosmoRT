/* Lock Wave 2 Tests — verify pty, tcp, net queue spinlock→mutex conversion */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

static int wait_child(long pid) {
    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    return WIFEXITED(ws) ? WEXITSTATUS(ws) : 99;
}

static void ms(int ms) {
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = ms * 1000000L };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
}

/* 1. PTY write/read — pty_t::lock now mutex */
static void test_pty_rw(void) {
    long fd = sc3(SYS_OPEN, (long)"/dev/tty1", O_WRONLY, 0);
    if (fd < 0) { pass("w2_pty_skip"); return; }
    long w = sc3(SYS_WRITE, fd, (long)"wave2\n", 6);
    check("w2_pty_write", w == 6);
    sc1(SYS_CLOSE, fd);
}

/* 2. TCP connect + data — tcp_hash_lock, rxring_t::lock now mutex */
static void test_tcp_data(void) {
    long s = sc3(SYS_SOCKET, 2, 1, 6);
    check("w2_tcp_socket", s >= 0);
    if (s < 0) return;
    sc1(SYS_CLOSE, s);
}

/* 3. DNS lookup — exercises packet queues (now mutex) */
static void test_dns(void) {
    long s = sc3(SYS_SOCKET, 2, 2, 17);
    check("w2_dns_socket", s >= 0);
    if (s >= 0) sc1(SYS_CLOSE, s);
}

/* 4. Pipe contention — two children write/read simultaneously */
static void test_pipe_contention(void) {
    int fds[2];
    check("w2_pipe_create", sc2(SYS_PIPE2, (long)fds, 0) == 0);

    long writer = sc0(SYS_FORK);
    if (writer == 0) {
        char buf[64];
        for (int i = 0; i < 64; i++) buf[i] = 'W';
        sc3(SYS_WRITE, fds[1], (long)buf, 64);
        sc1(SYS_EXIT, 0);
    }

    char rbuf[64] = {0};
    int total = 0;
    while (total < 64) {
        long r = sc3(SYS_READ, fds[0], (long)(rbuf + total), 64 - total);
        if (r <= 0) break;
        total += (int)r;
    }
    check("w2_pipe_read_64", total == 64);
    check("w2_pipe_data", rbuf[0] == 'W' && rbuf[63] == 'W');
    check("w2_pipe_child", wait_child(writer) == 0);
    sc1(SYS_CLOSE, fds[0]); sc1(SYS_CLOSE, fds[1]);
}

/* 5. mmap + fork + shared page — MM locks intact */
static void test_mmap_shared(void) {
    volatile int *sh = (volatile int *)(long)sc6(SYS_MMAP,
        0, 4096, PROT_RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check("w2_mmap_shared", (long)sh > 0);
    if ((long)sh <= 0) return;
    *sh = 0;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        *sh = 0xCAFE;
        sc1(SYS_EXIT, 0);
    }
    wait_child(pid);
    check("w2_shared_child_wrote", *sh == 0xCAFE);
    sc2(SYS_MUNMAP, (long)sh, 4096);
}

/* 6. Open + write + close stress — 4 children, same file */
static void test_file_stress(void) {
    long fd = sc3(SYS_OPEN, (long)"/tmp/w2_stress", O_CREAT | O_RDWR, 0644);
    check("w2_file_create", fd >= 0);
    sc1(SYS_CLOSE, fd);

    long pids[4];
    for (int i = 0; i < 4; i++) {
        pids[i] = sc0(SYS_FORK);
        if (pids[i] == 0) {
            for (int j = 0; j < 10; j++) {
                long f = sc3(SYS_OPEN, (long)"/tmp/w2_stress", O_WRONLY | O_APPEND, 0);
                if (f >= 0) {
                    char c = (char)('0' + i);
                    sc3(SYS_WRITE, f, (long)&c, 1);
                    sc1(SYS_CLOSE, f);
                }
            }
            sc1(SYS_EXIT, 0);
        }
    }
    int ok = 1;
    for (int i = 0; i < 4; i++)
        if (wait_child(pids[i]) != 0) ok = 0;
    check("w2_file_stress_ok", ok);

    fd = sc3(SYS_OPEN, (long)"/tmp/w2_stress", O_RDONLY, 0);
    char buf[64] = {0};
    long r = sc3(SYS_READ, fd, (long)buf, 64);
    sc1(SYS_CLOSE, fd);
    check("w2_file_stress_bytes", r == 40);
    sc2(SYS_UNLINK, (long)"/tmp/w2_stress", 0);
}

/* 7. Socket bind + connect — socket locks intact */
static void test_socket_bind(void) {
    long s1 = sc3(SYS_SOCKET, 2, 1, 0);
    check("w2_sock_create", s1 >= 0);
    long s2 = sc3(SYS_SOCKET, 2, 2, 0);
    check("w2_sock_udp", s2 >= 0);
    if (s1 >= 0) sc1(SYS_CLOSE, s1);
    if (s2 >= 0) sc1(SYS_CLOSE, s2);
}

/* 8. epoll multi-fd — epoll lock intact */
static void test_epoll_multi(void) {
    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("w2_epoll_create", epfd >= 0);

    int pfds[2];
    sc2(SYS_PIPE2, (long)pfds, 0);
    struct epoll_event ev = { .events = EPOLLIN, .data = 1 };
    long rc = sc4(SYS_EPOLL_CTL, epfd, 1, pfds[0], (long)&ev);
    check("w2_epoll_add", rc == 0);

    int pfds2[2];
    sc2(SYS_PIPE2, (long)pfds2, 0);
    struct epoll_event ev2 = { .events = EPOLLIN, .data = 2 };
    rc = sc4(SYS_EPOLL_CTL, epfd, 1, pfds2[0], (long)&ev2);
    check("w2_epoll_add2", rc == 0);

    sc3(SYS_WRITE, pfds[1], (long)"x", 1);
    struct epoll_event out[4];
    long n = sc4(SYS_EPOLL_WAIT, epfd, (long)out, 4, 100);
    check("w2_epoll_triggered", n >= 1);

    sc1(SYS_CLOSE, pfds[0]); sc1(SYS_CLOSE, pfds[1]);
    sc1(SYS_CLOSE, pfds2[0]); sc1(SYS_CLOSE, pfds2[1]);
    sc1(SYS_CLOSE, epfd);
}

/* 9. Concurrent socket pair — two children using unix sockets */
static void test_concurrent_unix(void) {
    int sv[2];
    long rc = sc4(SYS_SOCKETPAIR, 1, 1, 0, (long)sv);
    check("w2_sockpair", rc == 0);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        char buf[8] = {0};
        long r = sc3(SYS_READ, sv[1], (long)buf, 4);
        sc1(SYS_EXIT, (r == 4 && buf[0] == 'p') ? 0 : 1);
    }
    ms(10);
    sc3(SYS_WRITE, sv[0], (long)"ping", 4);
    check("w2_unix_child", wait_child(pid) == 0);
    sc1(SYS_CLOSE, sv[0]); sc1(SYS_CLOSE, sv[1]);
}

/* 10. Slab alloc/free stress — fork + mmap/munmap cycles */
static void test_slab_stress(void) {
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        for (int i = 0; i < 20; i++) {
            long a = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
            if (a > 0) {
                *(volatile int *)a = i;
                sc2(SYS_MUNMAP, a, 4096);
            }
        }
        sc1(SYS_EXIT, 0);
    }
    for (int i = 0; i < 20; i++) {
        long a = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
        if (a > 0) {
            *(volatile int *)a = i + 100;
            sc2(SYS_MUNMAP, a, 4096);
        }
    }
    check("w2_slab_stress", wait_child(pid) == 0);
}

TEST("lock_wave2", test_pty_rw);
TEST("lock_wave2", test_tcp_data);
TEST("lock_wave2", test_dns);
TEST("lock_wave2", test_pipe_contention);
TEST("lock_wave2", test_mmap_shared);
TEST("lock_wave2", test_file_stress);
TEST("lock_wave2", test_socket_bind);
TEST("lock_wave2", test_epoll_multi);
TEST("lock_wave2", test_concurrent_unix);
TEST("lock_wave2", test_slab_stress);
