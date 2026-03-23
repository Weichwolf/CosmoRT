/* CosmoRT init — exec bash with boot-test.sh as .bashrc */

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
    syscall1(80 /* chdir */, (long)"/home");

    char *envp[] = {
        "HOME=/home", "PATH=/usr/bin:/bin", "TERM=linux",
        "PWD=/home", "SHELL=/usr/bin/bash",
        "PS1=cosmo:\\w$ ",
        (char *)0
    };

    /* If /home/.boot exists, run it as boot-test script.
     * Otherwise start interactive bash. */
    long fd = syscall3(2 /* open */, (long)"/home/.boot", 0 /* O_RDONLY */, 0);
    if (fd >= 0) {
        syscall1(3 /* close */, fd);
        char *argv[] = { "/usr/bin/bash", "/home/.boot", (char *)0 };
        syscall3(59, (long)"/usr/bin/bash", (long)argv, (long)envp);
    }

    /* Interactive bash */
    char *argv_i[] = { "/usr/bin/bash", "--norc", "--noprofile", "-i", (char *)0 };
    syscall3(59, (long)"/usr/bin/bash", (long)argv_i, (long)envp);

    /* Fallback to sh */
    char *argv2[] = { "/usr/bin/sh", (char *)0 };
    syscall3(59, (long)"/usr/bin/sh", (long)argv2, (long)envp);

    puts("exec failed\n");
    syscall1(231, 1);
    __builtin_unreachable();
}
