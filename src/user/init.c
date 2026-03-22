/* CosmoRT init — boot into bash with test .bashrc */

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
    puts("CosmoRT init\n");

    char *envp[] = {
        "HOME=/home/cosmo",
        "PATH=/usr/bin:/bin",
        "TERM=linux",
        "PWD=/home/cosmo",
        "SHELL=/usr/bin/bash",
        (char *)0
    };

    /* Try bash (reads /home/cosmo/.bashrc) */
    char *bash_argv[] = { "bash", "--login", (char *)0 };
    syscall3(59, (long)"/usr/bin/bash", (long)bash_argv, (long)envp);

    /* Fallback: node directly */
    char *node_argv[] = { "node", "-e", "console.log('hello from CosmoRT')", (char *)0 };
    syscall3(59, (long)"/usr/bin/node", (long)node_argv, (long)envp);

    puts("init: no shell\n");
    syscall1(231, 1);
    __builtin_unreachable();
}
