/* CosmoRT VT Shell — minimal interactive echo on PTY
 * Used as init for qemu-gui. Blocking read loop on stdin.
 */

typedef unsigned long size_t;
typedef long ssize_t;

static long sc1(long n, long a1) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1) : "rcx","r11","memory");
    return ret;
}
static long sc3(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx","r11","memory");
    return ret;
}

#define SYS_read  0
#define SYS_write 1
#define SYS_exit_group 231

static void puts(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    sc3(SYS_write, 1, (long)s, (long)len);
}

static long sc4(long n, long a1, long a2, long a3, long a4) {
    long ret;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10) : "rcx","r11","memory");
    return ret;
}

static long sc2(long n, long a1, long a2) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2) : "rcx","r11","memory");
    return ret;
}

static void put_int(long v) {
    char buf[20]; int i = 0;
    if (v < 0) { sc3(SYS_write, 1, (long)"-", 1); v = -v; }
    do { buf[i++] = '0' + (char)(v % 10); v /= 10; } while (v);
    while (i--) sc3(SYS_write, 1, (long)&buf[i], 1);
}

void _start(void) {
    puts("CosmoRT Shell\n");
    sc1(80 /* chdir */, (long)"/home");

    /* Debug: test PTY read/write */
    puts("Testing stdin... type a char: ");
    char c = 0;
    long n = sc3(SYS_read, 0, (long)&c, 1);
    puts("read returned "); put_int(n); puts(" char=");
    if (n > 0) sc3(SYS_write, 1, (long)&c, 1);
    puts("\n");

    char *envp[] = {
        "HOME=/home", "PATH=/usr/bin:/bin", "TERM=linux",
        "PWD=/home", "SHELL=/usr/bin/bash",
        "PS1=cosmo:\\w$ ",
        (char *)0
    };

    /* Try sh first (simpler) */
    puts("Starting /usr/bin/sh...\n");
    char *argv_sh[] = { "/usr/bin/sh", (char *)0 };
    long ret = sc3(59, (long)"/usr/bin/sh", (long)argv_sh, (long)envp);
    puts("sh execve failed: "); put_int(ret); puts("\n");

    /* Try bash */
    puts("Starting /usr/bin/bash...\n");
    char *argv[] = { "/usr/bin/bash", "--norc", "--noprofile", "-i", (char *)0 };
    ret = sc3(59, (long)"/usr/bin/bash", (long)argv, (long)envp);
    puts("bash execve failed: "); put_int(ret); puts("\n");

    /* Fallback: minimal echo shell */
    puts("bash not found, fallback shell\ncosmo> ");
    for (;;) {
        char c;
        long n = sc3(SYS_read, 0, (long)&c, 1);
        if (n <= 0) continue;
        sc3(SYS_write, 1, (long)&c, 1);
        if (c == '\n') puts("cosmo> ");
    }
}
