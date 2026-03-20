/* CosmoRT init process — minimal, no libc, raw syscalls */

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

void _start(void) {
    /* write(1, "CosmoOS booted!\n", 16) */
    syscall3(1, 1, (long)"CosmoOS booted!\n", 16);

    /* exit_group(0) */
    syscall1(231, 0);

    __builtin_unreachable();
}
