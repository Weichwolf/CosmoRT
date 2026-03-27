/* CosmoRT Service Manager
 *
 * Freestanding (no libc). Spawns driver processes from a hardcoded
 * service list. Monitors via waitpid, auto-restarts crashed services.
 *
 * Runs as early userspace process. Kernel loads it into VFS at /bin/svcmgr.
 */

typedef unsigned long  uint64_t;
typedef long           int64_t;
typedef unsigned int   uint32_t;
typedef int            int32_t;
typedef unsigned char  uint8_t;
typedef unsigned long  size_t;

#define NULL ((void *)0)

/* ── Syscall wrappers ───────────────────────── */

static long sc1(long n, long a) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a):"rcx","r11","memory"); return r;
}
static long sc3(long n, long a, long b, long c) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r;
}
static long sc4(long n, long a, long b, long c, long d) {
    register long r10 __asm__("r10")=d;
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10):"rcx","r11","memory"); return r;
}

/* Syscall numbers */
#define SYS_write       1
#define SYS_fork        57
#define SYS_execve      59
#define SYS_exit_group  231
#define SYS_wait4       61
#define SYS_sched_yield 24

static void puts(const char *s) {
    int n = 0; while (s[n]) n++;
    sc3(SYS_write, 1, (long)s, n);
}

static void put_int(long v) {
    if (v < 0) { puts("-"); v = -v; }
    char buf[20]; int i = 0;
    do { buf[i++] = '0' + v % 10; v /= 10; } while (v);
    char out[20]; int j = 0;
    while (i--) out[j++] = buf[i];
    out[j] = 0;
    puts(out);
}

static long do_fork(void) {
    return sc1(SYS_fork, 0);
}

static long do_execve(const char *path) {
    return sc3(SYS_execve, (long)path, 0, 0);
}

static long do_waitpid(long pid, int *status, int options) {
    return sc4(SYS_wait4, pid, (long)status, options, 0);
}

static void do_exit(int code) {
    sc1(SYS_exit_group, code);
}

static void yield(void) {
    sc1(SYS_sched_yield, 0);
}

/* ── Service table ──────────────────────────── */

struct service {
    const char *name;
    const char *path;
    long pid;
    int restart;  /* 1 = auto-restart on crash */
};

static struct service services[] = {
    { "e1000d", "/bin/e1000d", 0, 1 },
};

#define N_SERVICES ((int)(sizeof(services) / sizeof(services[0])))

/* ── Start a service ────────────────────────── */

static void start_service(int idx) {
    long pid = do_fork();
    if (pid < 0) {
        puts("svcmgr: fork failed for ");
        puts(services[idx].name);
        puts("\n");
        return;
    }
    if (pid == 0) {
        /* Child: exec the service binary */
        do_execve(services[idx].path);
        /* execve failed */
        puts("svcmgr: execve failed for ");
        puts(services[idx].name);
        puts("\n");
        do_exit(1);
        __builtin_unreachable();
    }
    /* Parent */
    services[idx].pid = pid;
    puts("svcmgr: started ");
    puts(services[idx].name);
    puts(" pid=");
    put_int(pid);
    puts("\n");
}

/* ── Main ───────────────────────────────────── */

void _start_c(void) {
    puts("svcmgr: starting service manager\n");

    /* Start all services */
    for (int i = 0; i < N_SERVICES; i++)
        start_service(i);

    /* Monitor loop: waitpid + restart crashed services */
    for (;;) {
        int status = 0;
        long pid = do_waitpid(-1, &status, 0);

        if (pid > 0) {
            /* Find which service exited */
            for (int i = 0; i < N_SERVICES; i++) {
                if (services[i].pid == pid) {
                    puts("svcmgr: ");
                    puts(services[i].name);
                    puts(" exited (status=");
                    put_int(status);
                    puts(")\n");

                    if (services[i].restart) {
                        puts("svcmgr: restarting ");
                        puts(services[i].name);
                        puts("\n");
                        start_service(i);
                    }
                    break;
                }
            }
        } else {
            /* No children or error — yield and retry */
            yield();
        }
    }
}
