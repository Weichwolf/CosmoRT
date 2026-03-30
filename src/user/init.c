/* CosmoRT init — open console, exec INIT_PATH (default /sbin/init) */

#define SYS_OPEN   2
#define SYS_DUP2   33
#define SYS_CLOSE  3
#define SYS_EXECVE 59
#define SYS_WRITE  1
#define SYS_EXIT   231
#define SYS_SETSID 112
#define SYS_IOCTL  16
#define O_RDWR     2
#define TIOCSCTTY  0x540E

static long sc1(long n, long a) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a):"rcx","r11","memory"); return r;
}
static long sc2(long n, long a, long b) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b):"rcx","r11","memory"); return r;
}
static long sc3(long n, long a, long b, long c) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r;
}

void _start_c(void) {
    sc1(SYS_SETSID, 0);
    long fd = sc3(SYS_OPEN, (long)"/dev/console", O_RDWR, 0);
    if (fd >= 0) {
        sc2(SYS_DUP2, fd, 0);
        sc2(SYS_DUP2, fd, 1);
        sc2(SYS_DUP2, fd, 2);
        if (fd > 2) sc1(SYS_CLOSE, fd);
        sc3(SYS_IOCTL, 0, TIOCSCTTY, 1);
    }
    char *envp[] = { "HOME=/", "PATH=/bin:/usr/bin:/sbin:/usr/sbin",
                     "TERM=linux", (char *)0 };
    char *argv[] = { "/sbin/init", (char *)0 };
    sc3(SYS_EXECVE, (long)"/sbin/init", (long)argv, (long)envp);
    sc3(SYS_WRITE, 2, (long)"exec init failed\n", 17);
    sc1(SYS_EXIT, 1);
    __builtin_unreachable();
}
