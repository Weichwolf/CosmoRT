/* CosmoRT init — exec /bin/sh */

static long sc1(long n, long a) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a):"rcx","r11","memory"); return r;
}
static long sc3(long n, long a, long b, long c) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r;
}

void _start_c(void) {
    char *argv[] = { "sh", (char *)0 };
    char *envp[] = { "HOME=/", "PATH=/bin:/usr/bin", "TERM=linux", (char *)0 };
    sc3(59 /* execve */, (long)"/bin/sh", (long)argv, (long)envp);
    sc3(1 /* write */, 1, (long)"exec /bin/sh failed\n", 20);
    sc1(231 /* exit_group */, 1);
    __builtin_unreachable();
}
