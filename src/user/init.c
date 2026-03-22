/* CosmoRT init process — minimal, no libc, raw syscalls
 *
 * Boot sequence:
 *   1. Print banner
 *   2. Fork+exec: node -e "console.log('hello from CosmoRT')"
 *   3. Wait for node to finish
 *   4. Fork+exec: node /opt/claude-code/cli.js --version
 *   5. Wait for node to finish
 *   6. Exec /usr/bin/bash (interactive shell, replaces init)
 *   7. Fallback: /bin/bash, /bin/sh
 *   8. Halt if no shell
 */

#define SYS_WRITE      1
#define SYS_FORK       57
#define SYS_EXECVE     59
#define SYS_EXIT_GROUP 231
#define SYS_WAIT4      61
#define SYS_NANOSLEEP  35

static long syscall1(long num, long a1) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(num), "D"(a1)
                     : "rcx", "r11", "memory");
    return ret;
}

static long syscall2(long num, long a1, long a2) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(num), "D"(a1), "S"(a2)
                     : "rcx", "r11", "memory");
    return ret;
}

static long syscall3(long num, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(num), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

static long syscall4(long num, long a1, long a2, long a3, long a4) {
    long ret;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static void puts(const char *s) {
    int len = 0;
    while (s[len]) len++;
    syscall3(SYS_WRITE, 1, (long)s, len);
}

static void put_long(long v) {
    if (v < 0) {
        syscall3(SYS_WRITE, 1, (long)"-", 1);
        v = -v;
    }
    char buf[20];
    int i = 0;
    if (v == 0) buf[i++] = '0';
    else while (v > 0) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    while (i-- > 0) syscall3(SYS_WRITE, 1, (long)&buf[i], 1);
}

/* Fork, exec argv[0] with args, wait for child. Returns child exit status or -errno. */
static long fork_exec_wait(const char *path, char *const argv[], char *const envp[]) {
    long pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        puts("init: fork failed: ");
        put_long(pid);
        puts("\n");
        return pid;
    }
    if (pid == 0) {
        /* child */
        long ret = syscall3(SYS_EXECVE, (long)path, (long)argv, (long)envp);
        puts("init: execve failed: ");
        put_long(ret);
        puts(" path=");
        puts(path);
        puts("\n");
        syscall1(SYS_EXIT_GROUP, 127);
        __builtin_unreachable();
    }
    /* parent: wait */
    int status = 0;
    syscall4(SYS_WAIT4, pid, (long)&status, 0, 0);
    /* Extract exit code from wait status (bits 15..8) */
    return (status >> 8) & 0xFF;
}

void _start(void) {
    static char *envp[] = {
        "HOME=/home",
        "PATH=/usr/bin:/bin",
        "SHELL=/usr/bin/bash",
        "TERM=xterm-256color",
        "LANG=en_US.UTF-8",
        "NODE_NO_WARNINGS=1",
        (char *)0
    };

    puts("\n\033[1;36mCosmoOS\033[0m init\n\n");

    /* Step 1: Test bash --version (small binary, quick test) */
    puts("init: testing bash...\n");
    {
        static char *argv[] = {
            "/usr/bin/bash", "-c", "echo hello from CosmoRT bash",
            (char *)0
        };
        long rc = fork_exec_wait("/usr/bin/bash", argv, envp);
        puts("init: bash exited with status ");
        put_long(rc);
        puts("\n");
    }

    /* Step 2: Test Node.js */
    puts("init: testing node...\n");
    {
        static char *argv[] = {
            "/usr/bin/node", "-e", "console.log('hello from CosmoRT')",
            (char *)0
        };
        long rc = fork_exec_wait("/usr/bin/node", argv, envp);
        puts("init: node exited with status ");
        put_long(rc);
        puts("\n");
    }

    /* Step 3: Test Claude Code */
    puts("init: testing claude-code...\n");
    {
        static char *argv[] = {
            "/usr/bin/node", "/opt/claude-code/cli.js", "--version",
            (char *)0
        };
        long rc = fork_exec_wait("/usr/bin/node", argv, envp);
        puts("init: claude-code exited with status ");
        put_long(rc);
        puts("\n");
    }

    /* Step 3: Interactive shell (replaces init) */
    puts("init: starting bash...\n");
    {
        static char *argv[] = { "/usr/bin/bash", "--login", (char *)0 };
        syscall3(SYS_EXECVE, (long)"/usr/bin/bash", (long)argv, (long)envp);
    }
    /* Fallback paths */
    {
        static char *argv[] = { "/bin/bash", "--login", (char *)0 };
        syscall3(SYS_EXECVE, (long)"/bin/bash", (long)argv, (long)envp);
    }
    {
        static char *argv[] = { "/bin/sh", (char *)0 };
        syscall3(SYS_EXECVE, (long)"/bin/sh", (long)argv, (long)envp);
    }

    puts("init: no shell found, halting\n");
    syscall1(SYS_EXIT_GROUP, 1);
    __builtin_unreachable();
}
