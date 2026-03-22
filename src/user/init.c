/* CosmoRT init — test node deps */

static long syscall3(long num, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(num), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}
static long syscall1(long num, long a1) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(num), "D"(a1)
                     : "rcx", "r11", "memory");
    return ret;
}

static void puts(const char *s) {
    int len = 0;
    while (s[len]) len++;
    syscall3(1, 1, (long)s, len);
}

void _start(void) {
    puts("CosmoOS booted!\n");

    char *envp[] = {
        "HOME=/home", "PATH=/usr/bin", "TERM=linux",
        "PWD=/home", (char *)0
    };

    /* Run node.js dependency test (musl static binary) */
    char *argv[] = { "/usr/bin/node", (char *)0 };
    syscall3(59, (long)"/usr/bin/node", (long)argv, (long)envp);

    puts("exec failed\n");
    syscall1(231, 1);
    __builtin_unreachable();
}
