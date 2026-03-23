#include "ktest.h"

/* ── getrusage ───────────────────────────────── */

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)

struct test_timeval_ru { long tv_sec; long tv_usec; };

struct test_rusage {
    struct test_timeval_ru ru_utime;
    struct test_timeval_ru ru_stime;
    long ru_maxrss;
    long ru_ixrss, ru_idrss, ru_isrss;
    long ru_minflt, ru_majflt, ru_nswap;
    long ru_inblock, ru_oublock;
    long ru_msgsnd, ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw, ru_nivcsw;
};

static void test_getrusage(void) {
    puts("\n[getrusage]\n");

    struct test_rusage ru;
    long r = sc2(SYS_GETRUSAGE, RUSAGE_SELF, (long)&ru);
    check_val("getrusage returns 0", r, 0);
    check("ru_maxrss > 0", ru.ru_maxrss > 0);
    check("ru_utime non-zero", ru.ru_utime.tv_sec > 0 || ru.ru_utime.tv_usec > 0);

    /* RUSAGE_CHILDREN: all zeros */
    struct test_rusage ruc;
    r = sc2(SYS_GETRUSAGE, RUSAGE_CHILDREN, (long)&ruc);
    check_val("getrusage children returns 0", r, 0);
    check_val("children ru_maxrss = 0", ruc.ru_maxrss, 0);

    /* Invalid who */
    r = sc2(SYS_GETRUSAGE, 99, (long)&ru);
    check_val("getrusage invalid who → -EINVAL", r, -EINVAL);
}

TEST("getrusage", test_getrusage);

/* ── times ───────────────────────────────────── */

struct test_tms {
    long tms_utime, tms_stime, tms_cutime, tms_cstime;
};

static void test_times(void) {
    puts("\n[times]\n");

    struct test_tms tms;
    long r = sc1(SYS_TIMES, (long)&tms);
    check("times returns ticks > 0", r > 0);
    check("tms_utime > 0", tms.tms_utime > 0);
    check_val("tms_stime = 0", tms.tms_stime, 0);
    check_val("tms_cutime = 0", tms.tms_cutime, 0);
    check_val("tms_cstime = 0", tms.tms_cstime, 0);
    /* Return value and tms_utime should be close */
    long diff = r - tms.tms_utime;
    if (diff < 0) diff = -diff;
    check("tms_utime close to return", diff < 10);
}

TEST("times", test_times);

/* ── sigaltstack ─────────────────────────────── */

#define SS_DISABLE 2

struct test_stack_t {
    uint64_t ss_sp;
    int32_t  ss_flags;
    int32_t  _pad;
    uint64_t ss_size;
};

static void test_sigaltstack(void) {
    puts("\n[sigaltstack]\n");

    /* Get old stack */
    struct test_stack_t oss;
    long r = sc2(SYS_SIGALTSTACK, 0, (long)&oss);
    check_val("sigaltstack get returns 0", r, 0);

    /* Set alternate stack */
    long alt = sc6(SYS_MMAP, 0, 16384, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap altstack", alt > 0);
    if (alt <= 0) return;

    struct test_stack_t ss;
    ss.ss_sp = (uint64_t)alt;
    ss.ss_flags = 0;
    ss._pad = 0;
    ss.ss_size = 16384;
    r = sc2(SYS_SIGALTSTACK, (long)&ss, (long)&oss);
    check_val("sigaltstack set returns 0", r, 0);

    /* Verify */
    struct test_stack_t oss2;
    r = sc2(SYS_SIGALTSTACK, 0, (long)&oss2);
    check_val("sigaltstack verify returns 0", r, 0);
    check("ss_sp matches", oss2.ss_sp == (uint64_t)alt);
    check("ss_size matches", oss2.ss_size == 16384);

    /* Disable */
    struct test_stack_t dis;
    dis.ss_sp = 0;
    dis.ss_flags = SS_DISABLE;
    dis._pad = 0;
    dis.ss_size = 0;
    r = sc2(SYS_SIGALTSTACK, (long)&dis, 0);
    check_val("sigaltstack disable", r, 0);

    /* Verify disabled */
    r = sc2(SYS_SIGALTSTACK, 0, (long)&oss2);
    check("disabled ss_flags", oss2.ss_flags == SS_DISABLE);

    sc2(SYS_MUNMAP, alt, 16384);
}

TEST("sigaltstack", test_sigaltstack);

/* ── signal constants ────────────────────────── */

static void test_signal_constants(void) {
    puts("\n[signal constants]\n");

    /* Verify a selection of signal constants are defined correctly
     * by sending signal 0 (check permission only) to self */
    long pid = sc0(SYS_GETPID);

    /* These should all succeed (sig 0 = validation only) */
    check_val("kill sig=0", sc2(SYS_KILL, pid, 0), 0);

    /* Install SIG_IGN for each signal and send it to self */
    struct { void *handler; uint64_t flags; void *restorer; uint64_t mask; } sa;
    sa.handler = (void *)1; /* SIG_IGN */
    sa.flags = 0;
    sa.restorer = 0;
    sa.mask = 0;

    /* SIGHUP=1 */
    long r = sc4(SYS_RT_SIGACTION, 1, (long)&sa, 0, 8);
    check_val("sigaction SIGHUP", r, 0);
    r = sc2(SYS_KILL, pid, 1);
    check_val("kill SIGHUP", r, 0);

    /* SIGABRT=6 */
    r = sc4(SYS_RT_SIGACTION, 6, (long)&sa, 0, 8);
    check_val("sigaction SIGABRT", r, 0);

    /* SIGBUS=7 */
    r = sc4(SYS_RT_SIGACTION, 7, (long)&sa, 0, 8);
    check_val("sigaction SIGBUS", r, 0);

    /* SIGWINCH=28 */
    r = sc4(SYS_RT_SIGACTION, 28, (long)&sa, 0, 8);
    check_val("sigaction SIGWINCH", r, 0);

    /* SIGSYS=31 */
    r = sc4(SYS_RT_SIGACTION, 31, (long)&sa, 0, 8);
    check_val("sigaction SIGSYS", r, 0);
}

TEST("signal constants", test_signal_constants);

/* ── poll timeout=-1 ─────────────────────────── */

struct test_pollfd { int fd; short events; short revents; };
#define T_POLLIN  0x0001

static void test_poll_infinite(void) {
    puts("\n[poll timeout=-1]\n");

    int pipefd[2];
    long r = sc2(SYS_PIPE2, (long)pipefd, 0);
    check_val("pipe2", r, 0);
    if (r < 0) return;

    /* Write data so poll returns immediately */
    sc3(SYS_WRITE, pipefd[1], (long)"x", 1);

    struct test_pollfd pfd;
    pfd.fd = pipefd[0];
    pfd.events = T_POLLIN;
    pfd.revents = 0;
    r = sc3(SYS_POLL, (long)&pfd, 1, -1);
    check("poll(-1) returns ready", r >= 1);
    check("POLLIN set", (pfd.revents & T_POLLIN) != 0);

    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

TEST("poll timeout=-1", test_poll_infinite);

/* ── EPOLLET ─────────────────────────────────── */

#define T_EPOLLIN  0x001
#define T_EPOLLET  (1U << 31)
#define T_EPOLL_CTL_ADD 1

struct test_epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

static void test_epollet(void) {
    puts("\n[EPOLLET]\n");

    long efd = sc2(SYS_EVENTFD2, 0, 0);
    check("eventfd2", efd >= 0);
    if (efd < 0) return;

    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) { sc1(SYS_CLOSE, efd); return; }

    struct test_epoll_event ev;
    ev.events = T_EPOLLIN | T_EPOLLET;
    ev.data = 42;
    long r = sc4(SYS_EPOLL_CTL, epfd, T_EPOLL_CTL_ADD, efd, (long)&ev);
    check_val("epoll_ctl ADD ET", r, 0);

    /* Write to eventfd */
    uint64_t val = 1;
    sc3(SYS_WRITE, efd, (long)&val, 8);

    /* First wait: should see event */
    struct test_epoll_event out;
    r = sc4(SYS_EPOLL_WAIT, epfd, (long)&out, 1, 0);
    check_val("epoll_wait ET first", r, 1);

    /* Second wait: edge consumed, should return 0 */
    r = sc4(SYS_EPOLL_WAIT, epfd, (long)&out, 1, 0);
    check_val("epoll_wait ET second = 0", r, 0);

    sc1(SYS_CLOSE, epfd);
    sc1(SYS_CLOSE, efd);
}

TEST("EPOLLET", test_epollet);

/* ── /proc/cpuinfo Linux format ──────────────── */

static void test_proc_cpuinfo_linux(void) {
    puts("\n[/proc/cpuinfo format]\n");

    long fd = sc3(SYS_OPEN, (long)"/proc/cpuinfo", O_RDONLY, 0);
    check("open /proc/cpuinfo", fd >= 0);
    if (fd < 0) return;

    char buf[512] = {0};
    long r = sc3(SYS_READ, fd, (long)buf, 511);
    check("read cpuinfo > 0", r > 0);

    /* Should contain "processor\t: 0" */
    int found_proc = 0, found_model = 0, found_mhz = 0;
    for (int i = 0; i < (int)r - 10; i++) {
        if (buf[i]=='p' && buf[i+1]=='r' && buf[i+2]=='o' && buf[i+3]=='c' &&
            buf[i+4]=='e' && buf[i+5]=='s' && buf[i+6]=='s' && buf[i+7]=='o' &&
            buf[i+8]=='r') found_proc = 1;
        if (buf[i]=='m' && buf[i+1]=='o' && buf[i+2]=='d' && buf[i+3]=='e' &&
            buf[i+4]=='l') found_model = 1;
        if (buf[i]=='c' && buf[i+1]=='p' && buf[i+2]=='u' && buf[i+3]==' ' &&
            buf[i+4]=='M' && buf[i+5]=='H' && buf[i+6]=='z') found_mhz = 1;
    }
    check("has processor field", found_proc);
    check("has model name", found_model);
    check("has cpu MHz", found_mhz);

    sc1(SYS_CLOSE, fd);
}

TEST("/proc/cpuinfo format", test_proc_cpuinfo_linux);

/* ── /proc/meminfo Linux format ──────────────── */

static void test_proc_meminfo_linux(void) {
    puts("\n[/proc/meminfo format]\n");

    long fd = sc3(SYS_OPEN, (long)"/proc/meminfo", O_RDONLY, 0);
    check("open /proc/meminfo", fd >= 0);
    if (fd < 0) return;

    char buf[512] = {0};
    long r = sc3(SYS_READ, fd, (long)buf, 511);
    check("read meminfo > 0", r > 0);

    /* Should contain MemAvailable, Buffers, Cached */
    int found_avail = 0, found_buf = 0, found_cache = 0;
    for (int i = 0; i < (int)r - 8; i++) {
        if (buf[i]=='M' && buf[i+1]=='e' && buf[i+2]=='m' &&
            buf[i+3]=='A' && buf[i+4]=='v') found_avail = 1;
        if (buf[i]=='B' && buf[i+1]=='u' && buf[i+2]=='f' &&
            buf[i+3]=='f' && buf[i+4]=='e') found_buf = 1;
        if (buf[i]=='C' && buf[i+1]=='a' && buf[i+2]=='c' &&
            buf[i+3]=='h' && buf[i+4]=='e') found_cache = 1;
    }
    check("has MemAvailable", found_avail);
    check("has Buffers", found_buf);
    check("has Cached", found_cache);

    sc1(SYS_CLOSE, fd);
}

TEST("/proc/meminfo format", test_proc_meminfo_linux);
