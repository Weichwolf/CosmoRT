/* CosmoRT Claude Code init — freestanding, no libc, raw syscalls
 *
 * Boot sequence:
 *   1. mount /proc, /sys, /dev, /tmp, /dev/pts (stubs if unsupported)
 *   2. set up /etc/resolv.conf, /etc/hosts
 *   3. exec node /opt/claude-code/cli.js
 *   4. fallback: exec /usr/bin/bash
 *   5. fallback: exec /usr/bin/sh
 *   6. fallback: print error, halt
 */

/* ── Syscall wrappers ───────────────────────────────── */

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

static long syscall5(long num, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(num), "D"(a1), "S"(a2), "d"(a3),
                       "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}

/* Syscall numbers (x86_64) */
#define SYS_write        1
#define SYS_open         2
#define SYS_close        3
#define SYS_execve      59
#define SYS_exit_group  231
#define SYS_mount       165
#define SYS_mkdir       83
#define SYS_chdir       80
#define SYS_sethostname 170

/* ── String helpers ─────────────────────────────────── */

static int slen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void puts(const char *s) {
    syscall3(SYS_write, 1, (long)s, slen(s));
}

/* ── Filesystem setup ───────────────────────────────── */

static void do_mkdir(const char *path) {
    syscall2(SYS_mkdir, (long)path, 0755);
}

static void do_mount(const char *src, const char *target, const char *fstype) {
    long rc = syscall5(SYS_mount, (long)src, (long)target, (long)fstype, 0, 0);
    if (rc < 0) {
        puts("  mount ");
        puts(target);
        puts(": not supported (ok)\n");
    } else {
        puts("  mount ");
        puts(target);
        puts(": ok\n");
    }
}

static void write_file(const char *path, const char *content) {
    /* O_WRONLY | O_CREAT | O_TRUNC = 0x241 */
    long fd = syscall3(SYS_open, (long)path, 0x241, 0644);
    if (fd >= 0) {
        syscall3(SYS_write, fd, (long)content, slen(content));
        syscall1(SYS_close, fd);
    }
}

/* ── Main ───────────────────────────────────────────── */

void _start(void) {
    puts("\n=== CosmoRT Claude Code Init ===\n\n");

    /* Skip mounts/config for now — just try execve directly */
    const char *envp[] = {
        "HOME=/home/cosmo",
        "PATH=/usr/bin:/bin",
        "TERM=xterm-256color",
        (void *)0
    };

    puts("Trying bash...\n");
    {
        const char *argv[] = { "/usr/bin/bash", "-c", "echo hello from CosmoRT bash", (void *)0 };
        syscall3(SYS_execve, (long)argv[0], (long)argv, (long)envp);
    }

    /* Try 4: exec sh */
    puts("Bash failed, trying sh...\n");
    {
        const char *argv[] = { "/usr/bin/sh", (void *)0 };
        syscall3(SYS_execve, (long)argv[0], (long)argv, (long)envp);
    }

    /* All failed */
    puts("\n!!! FATAL: No shell available. Halting. !!!\n");
    syscall1(SYS_exit_group, 1);
    __builtin_unreachable();
}
