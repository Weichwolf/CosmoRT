/* CosmoRT Phase 15 — Network namespace ktests.
 *
 * Cover the user-visible behaviour of CLONE_NEWNET, unshare(CLONE_NEWNET),
 * setns(CLONE_NEWNET), per-NS port isolation, per-NS abstract AF_UNIX,
 * /proc/<pid>/ns/net symlink format, and per-NS sysctl tags. */
#include "ktest.h"

#define CLONE_NEWNET   0x40000000
#define CLONE_VM       0x00000100
#define SIGCHLD        17
#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

#define AF_INET        2
#define AF_UNIX        1
#define SOCK_STREAM    1
#define SO_REUSEADDR   2
#define SOL_SOCKET     1

#define NSNET_PORT_PRIMARY   45123  /* avoid colliding with stack-tests */
#define NSNET_PORT_SECONDARY 45124
#define NSNET_PORT_LO        46100

struct k_sockaddr_in { uint16_t family; uint16_t port; uint32_t addr; char pad[8]; };
struct k_sockaddr_un { uint16_t family; char path[108]; };

static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

/* 1: CLONE_NEWNET fork → child sees its own loopback (127.0.0.1 reachable
 * via the per-NS lo). We probe by binding TCP/lo:port in child without
 * EADDRINUSE — only possible if a fresh netif list exists. */
static void test_newnet_fork_owns_loopback(void) {
    puts("\n[net_ns: CLONE_NEWNET fork sees own lo]\n");

    /* Parent binds NSNET_PORT_PRIMARY in init NS. */
    int pfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check_ge("parent socket", pfd, 0);
    int reuse = 1;
    sc5(SYS_SETSOCKOPT, pfd, SOL_SOCKET, SO_REUSEADDR, (long)&reuse, sizeof(int));
    struct k_sockaddr_in sa = {0};
    sa.family = AF_INET; sa.port = bswap16(NSNET_PORT_PRIMARY); sa.addr = 0;
    long r = sc3(SYS_BIND, pfd, (long)&sa, sizeof(sa));
    check_val("parent bind primary", r, 0);

    int pipefd[2] = { -1, -1 };
    sc1(SYS_PIPE, (long)pipefd);
    long pid = sc5(SYS_CLONE, CLONE_NEWNET | SIGCHLD, 0, 0, 0, 0);
    check_ge("clone(CLONE_NEWNET)", pid, 0);
    if (pid == 0) {
        /* Child: bind same port, must succeed (own NS). */
        int cfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
        struct k_sockaddr_in csa = {0};
        csa.family = AF_INET; csa.port = bswap16(NSNET_PORT_PRIMARY); csa.addr = 0;
        long cr = sc3(SYS_BIND, cfd, (long)&csa, sizeof(csa));
        long out = (cr == 0) ? 1 : -1;
        sc3(SYS_WRITE, pipefd[1], (long)&out, sizeof(out));
        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT_GROUP, 0);
    }
    sc1(SYS_CLOSE, pipefd[1]);
    long got = -1;
    sc3(SYS_READ, pipefd[0], (long)&got, sizeof(got));
    sc1(SYS_CLOSE, pipefd[0]);
    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check_val("child bound primary in new NS", got, 1);
    sc1(SYS_CLOSE, pfd);
}

/* 2: 2 children in 2 separate NS bind same port concurrently — no EADDRINUSE.
 * Strategy: spawn each child sequentially but don't ack — child binds and
 * holds the port via a shared-memory marker in /tmp. We use /tmp as a tiny
 * cross-NS "rendezvous" via files instead of pipe-ack to avoid pipe-blocking
 * complexity with concurrent NS-children. */
static void test_two_ns_share_port(void) {
    puts("\n[net_ns: two NS bind same port]\n");

    /* Phase 1: child A in fresh NS binds port and exits with status=0 on success. */
    long ca = sc5(SYS_CLONE, CLONE_NEWNET | SIGCHLD, 0, 0, 0, 0);
    if (ca == 0) {
        int fd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
        struct k_sockaddr_in s = {0};
        s.family = AF_INET; s.port = bswap16(NSNET_PORT_SECONDARY); s.addr = 0;
        long r = sc3(SYS_BIND, fd, (long)&s, sizeof(s));
        sc1(SYS_EXIT_GROUP, r == 0 ? 0 : 1);
    }
    int sta = 0;
    sc4(SYS_WAIT4, ca, (long)&sta, 0, 0);
    check_val("ns#A bind exit code", WEXITSTATUS(sta), 0);

    /* Phase 2: child B in *another* fresh NS binds the *same* port. Since A
     * already exited (its NS gone), the port should also be free in init NS,
     * but we keep the test focused on per-NS allocation: B binds in own NS. */
    long cb = sc5(SYS_CLONE, CLONE_NEWNET | SIGCHLD, 0, 0, 0, 0);
    if (cb == 0) {
        int fd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
        struct k_sockaddr_in s = {0};
        s.family = AF_INET; s.port = bswap16(NSNET_PORT_SECONDARY); s.addr = 0;
        long r = sc3(SYS_BIND, fd, (long)&s, sizeof(s));
        sc1(SYS_EXIT_GROUP, r == 0 ? 0 : 1);
    }
    int stb = 0;
    sc4(SYS_WAIT4, cb, (long)&stb, 0, 0);
    check_val("ns#B bind exit code", WEXITSTATUS(stb), 0);

    /* Phase 3: parent itself binds the same port concurrently with a child
     * that holds it in its NS. The CRITICAL test for NS-isolation. */
    int pfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    struct k_sockaddr_in psa = {0};
    psa.family = AF_INET; psa.port = bswap16(NSNET_PORT_SECONDARY); psa.addr = 0;
    long pr = sc3(SYS_BIND, pfd, (long)&psa, sizeof(psa));
    check_val("parent bind in init NS", pr, 0);

    long cc = sc5(SYS_CLONE, CLONE_NEWNET | SIGCHLD, 0, 0, 0, 0);
    if (cc == 0) {
        /* Same port, different NS — must succeed. */
        int fd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
        struct k_sockaddr_in s = {0};
        s.family = AF_INET; s.port = bswap16(NSNET_PORT_SECONDARY); s.addr = 0;
        long r = sc3(SYS_BIND, fd, (long)&s, sizeof(s));
        sc1(SYS_EXIT_GROUP, r == 0 ? 0 : 1);
    }
    int stc = 0;
    sc4(SYS_WAIT4, cc, (long)&stc, 0, 0);
    check_val("child binds same port concurrently in own NS",
              WEXITSTATUS(stc), 0);
    sc1(SYS_CLOSE, pfd);
}

/* 3: unshare(CLONE_NEWNET) trennt netif list. The current task moves
 * into a fresh NS; binding a port must succeed even if the parent NS
 * already has it. We verify in a fork-child to keep the parent test in
 * its original NS. */
static void test_unshare_separates_netif(void) {
    puts("\n[net_ns: unshare(CLONE_NEWNET) separates]\n");

    int pfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    struct k_sockaddr_in sa = {0};
    sa.family = AF_INET; sa.port = bswap16(NSNET_PORT_LO); sa.addr = 0;
    long r = sc3(SYS_BIND, pfd, (long)&sa, sizeof(sa));
    check_val("parent bind", r, 0);

    int pipe2[2] = {-1,-1};
    sc1(SYS_PIPE, (long)pipe2);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        long ur = sc1(SYS_UNSHARE, CLONE_NEWNET);
        int cfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
        struct k_sockaddr_in csa = {0};
        csa.family = AF_INET; csa.port = bswap16(NSNET_PORT_LO); csa.addr = 0;
        long cr = sc3(SYS_BIND, cfd, (long)&csa, sizeof(csa));
        long out = (ur == 0 && cr == 0) ? 1 : -1;
        sc3(SYS_WRITE, pipe2[1], (long)&out, sizeof(out));
        sc1(SYS_EXIT_GROUP, 0);
    }
    sc1(SYS_CLOSE, pipe2[1]);
    long ok = -1;
    sc3(SYS_READ, pipe2[0], (long)&ok, sizeof(ok));
    sc1(SYS_CLOSE, pipe2[0]);
    sc4(SYS_WAIT4, pid, 0, 0, 0);
    check_val("child after unshare bound port", ok, 1);
    sc1(SYS_CLOSE, pfd);
}

/* 4: setns(fd, CLONE_NEWNET) re-bindet net_ns. We open
 * /proc/self/ns/net before unshare, then unshare, then setns back. After
 * setns the original NS-id must reappear in /proc/self/ns/net readlink. */
static void test_setns_rebinds(void) {
    puts("\n[net_ns: setns rebinds]\n");

    long fd = sc4(SYS_OPENAT, -100, (long)"/proc/self/ns/net", 0, 0);
    check_ge("open ns/net pre-unshare", fd, 0);
    if (fd < 0) return;

    char before[64] = {0};
    long bl = sc4(SYS_READLINKAT, -100, (long)"/proc/self/ns/net",
                  (long)before, sizeof(before));
    check("readlink ns/net pre", bl > 0);

    /* Move into a new NS for the duration of this test. */
    long ur = sc1(SYS_UNSHARE, CLONE_NEWNET);
    check_val("unshare(CLONE_NEWNET)", ur, 0);

    char during[64] = {0};
    long dl = sc4(SYS_READLINKAT, -100, (long)"/proc/self/ns/net",
                  (long)during, sizeof(during));
    check("readlink ns/net post-unshare", dl > 0);
    /* Different NS → readlink output must differ. */
    int diff = 0;
    if (bl == dl) {
        for (int i = 0; i < bl; i++) if (before[i] != during[i]) { diff = 1; break; }
    } else {
        diff = 1;
    }
    check("ns/net id changed after unshare", diff);

    /* Hop back via setns. */
    long sr = sc2(SYS_SETNS, fd, CLONE_NEWNET);
    check_val("setns(fd, CLONE_NEWNET)", sr, 0);

    char after[64] = {0};
    long al = sc4(SYS_READLINKAT, -100, (long)"/proc/self/ns/net",
                  (long)after, sizeof(after));
    int same = (al == bl);
    if (same) {
        for (int i = 0; i < bl; i++) if (before[i] != after[i]) { same = 0; break; }
    }
    check("ns/net id restored after setns", same);
    sc1(SYS_CLOSE, fd);
}

/* 5: /proc/self/ns/net symlink existiert + Format "net:[<id>]". */
static void test_proc_ns_net_format(void) {
    puts("\n[net_ns: /proc/self/ns/net format]\n");
    char buf[64] = {0};
    long n = sc4(SYS_READLINKAT, -100, (long)"/proc/self/ns/net",
                 (long)buf, sizeof(buf));
    check("readlink succeeded", n > 0);
    if (n <= 0) return;
    /* Prefix must be "net:[" */
    int ok = (n >= 6 && buf[0]=='n' && buf[1]=='e' && buf[2]=='t' &&
              buf[3]==':' && buf[4]=='[' && buf[n-1]==']');
    check("symlink format net:[<id>]", ok);
}

/* 6: TCP-connect cross-NS schlägt fehl (loopback in NS-A nicht in NS-B). */
static void test_tcp_cross_ns_refused(void) {
    puts("\n[net_ns: cross-NS TCP connect refused]\n");

    /* Plan: parent bindet+listen auf 127.0.0.1:NSNET_PORT_PRIMARY+50,
     * child unshare's und versucht zu connecten — muss ECONNREFUSED. */
    int lfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { check("listener", 0); return; }
    int reuse = 1;
    sc5(SYS_SETSOCKOPT, lfd, SOL_SOCKET, SO_REUSEADDR, (long)&reuse, sizeof(int));
    struct k_sockaddr_in la = {0};
    la.family = AF_INET; la.port = bswap16(NSNET_PORT_PRIMARY + 50);
    la.addr = (uint32_t)127 | (1u << 24); /* 127.0.0.1 in BE wire layout */
    long br = sc3(SYS_BIND, lfd, (long)&la, sizeof(la));
    check_val("listener bind", br, 0);
    long lr = sc2(SYS_LISTEN, lfd, 4);
    check_val("listen", lr, 0);

    int pipefd[2] = {-1, -1};
    sc1(SYS_PIPE, (long)pipefd);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_UNSHARE, CLONE_NEWNET);
        int cfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
        struct k_sockaddr_in ra = {0};
        ra.family = AF_INET; ra.port = bswap16(NSNET_PORT_PRIMARY + 50);
        ra.addr = (uint32_t)127 | (1u << 24);
        long cr = sc3(SYS_CONNECT, cfd, (long)&ra, sizeof(ra));
        sc3(SYS_WRITE, pipefd[1], (long)&cr, sizeof(cr));
        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT_GROUP, 0);
    }
    sc1(SYS_CLOSE, pipefd[1]);
    long got = 0;
    sc3(SYS_READ, pipefd[0], (long)&got, sizeof(got));
    sc1(SYS_CLOSE, pipefd[0]);
    sc4(SYS_WAIT4, pid, 0, 0, 0);
    /* Linux: ECONNREFUSED (-111). Wir akzeptieren auch -110/-101 (ETIMEDOUT/
     * ENETUNREACH) als negativ; entscheidend ist != 0. */
    check("cross-NS connect failed", got < 0);
    sc1(SYS_CLOSE, lfd);
}

/* 7: Inherit nach fork ohne CLONE_NEWNET teilt netif: ns/net IDs must
 * match between parent and child. */
static void test_fork_inherits_ns(void) {
    puts("\n[net_ns: fork without flag shares NS]\n");

    char parent_id[64] = {0};
    long pl = sc4(SYS_READLINKAT, -100, (long)"/proc/self/ns/net",
                  (long)parent_id, sizeof(parent_id));
    check("parent readlink", pl > 0);

    int pipefd[2] = {-1, -1};
    sc1(SYS_PIPE, (long)pipefd);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        char cid[64] = {0};
        long cl = sc4(SYS_READLINKAT, -100, (long)"/proc/self/ns/net",
                      (long)cid, sizeof(cid));
        sc3(SYS_WRITE, pipefd[1], (long)&cl, sizeof(cl));
        sc3(SYS_WRITE, pipefd[1], (long)cid, cl > 0 ? cl : 0);
        sc1(SYS_EXIT_GROUP, 0);
    }
    sc1(SYS_CLOSE, pipefd[1]);
    long cl = 0;
    sc3(SYS_READ, pipefd[0], (long)&cl, sizeof(cl));
    char child_id[64] = {0};
    if (cl > 0 && cl < (long)sizeof(child_id))
        sc3(SYS_READ, pipefd[0], (long)child_id, cl);
    sc1(SYS_CLOSE, pipefd[0]);
    sc4(SYS_WAIT4, pid, 0, 0, 0);
    int eq = (cl == pl);
    if (eq) for (int i = 0; i < cl; i++) if (parent_id[i] != child_id[i]) { eq = 0; break; }
    check("child inherits parent's net_ns id", eq);
}

/* 8: AF_UNIX abstract socket per-NS isoliert. */
static void test_af_unix_abstract_per_ns(void) {
    puts("\n[net_ns: AF_UNIX abstract per-NS]\n");
    static const char abstract_name[] = {0, 'c', 'o', 's', 'm', 'o', '_',
                                         'n', 's', 't', 'e', 's', 't'};
    int abstract_len = (int)sizeof(abstract_name); /* 13 */

    /* Parent binds in init NS. */
    int pfd = (int)sc3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
    check_ge("parent unix sock", pfd, 0);
    struct k_sockaddr_un ua = {0};
    ua.family = AF_UNIX;
    for (int i = 0; i < abstract_len; i++) ua.path[i] = abstract_name[i];
    long pr = sc3(SYS_BIND, pfd, (long)&ua, 2 + abstract_len);
    check_val("parent abstract bind", pr, 0);

    int pipefd[2] = {-1,-1};
    sc1(SYS_PIPE, (long)pipefd);
    long pid = sc5(SYS_CLONE, CLONE_NEWNET | SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        int cfd = (int)sc3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
        struct k_sockaddr_un cua = {0};
        cua.family = AF_UNIX;
        for (int i = 0; i < abstract_len; i++) cua.path[i] = abstract_name[i];
        long cr = sc3(SYS_BIND, cfd, (long)&cua, 2 + abstract_len);
        sc3(SYS_WRITE, pipefd[1], (long)&cr, sizeof(cr));
        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT_GROUP, 0);
    }
    sc1(SYS_CLOSE, pipefd[1]);
    long got = -99;
    sc3(SYS_READ, pipefd[0], (long)&got, sizeof(got));
    sc1(SYS_CLOSE, pipefd[0]);
    sc4(SYS_WAIT4, pid, 0, 0, 0);
    check_val("abstract bind in new NS succeeded", got, 0);
    sc1(SYS_CLOSE, pfd);
}

TEST("net_ns/newnet-fork-loopback",     test_newnet_fork_owns_loopback);
TEST("net_ns/two-ns-share-port",        test_two_ns_share_port);
TEST("net_ns/unshare-separates-netif",  test_unshare_separates_netif);
TEST("net_ns/setns-rebinds",            test_setns_rebinds);
TEST("net_ns/proc-ns-net-format",       test_proc_ns_net_format);
TEST("net_ns/cross-ns-tcp-refused",     test_tcp_cross_ns_refused);
TEST("net_ns/fork-inherits-ns",         test_fork_inherits_ns);
TEST("net_ns/af-unix-abstract-per-ns",  test_af_unix_abstract_per_ns);
