/* CosmoRT VT Shell — minimal interactive echo on PTY
 * Used as init for qemu-gui. Runs 61 tests first, then interactive prompt.
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

void _start(void) {
    puts("CosmoRT\n\ncosmo> ");
    for (;;) {
        char c;
        long n = sc3(SYS_read, 0, (long)&c, 1);
        if (n <= 0) continue;
        sc3(SYS_write, 1, (long)&c, 1);
        if (c == '\n') puts("cosmo> ");
    }
}
