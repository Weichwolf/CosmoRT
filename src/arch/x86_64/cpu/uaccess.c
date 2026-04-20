/* x86_64 user-memory access primitives - extable-based recovery.
 *
 * Each helper runs a single user-touching instruction annotated via
 * _ASM_EXTABLE. On page fault the handler rewrites RIP to the fixup
 * label, which sets -EFAULT and jumps to the epilogue. Clobbered
 * registers are limited to rcx/rsi/rdi (rep movsb) plus the output -
 * no jmpbuf, no thread state.
 *
 * Bounds check filters obviously bad pointers (kernel half, overflow).
 * The on-fault path still covers unmapped-but-valid user addresses.
 *
 * aarch64 gets its own implementation with ldtr/sttr + PAN handling.
 */

#include "uaccess.h"
#include "mm/extable.h"
#include "linux/abi.h"
#include <stddef.h>

__attribute__((hot))
int copy_from_user(void *kernel_dst, const void *user_src, size_t len) {
    if (__builtin_expect(!user_ok((uint64_t)user_src, len), 0)) return -EFAULT;
    int ret = 0;
    __asm__ volatile(
        "1: rep movsb\n"
        "2:\n"
        _ASM_EXTABLE(1b, 3f)
        ".pushsection .text.fixup, \"ax\"\n"
        "3: movl %[efault], %[ret]\n"
        "   jmp 2b\n"
        ".popsection\n"
        : [ret] "+r"(ret), "+D"(kernel_dst), "+S"(user_src), "+c"(len)
        : [efault] "i"(-EFAULT)
        : "memory");
    return ret;
}

__attribute__((hot))
int copy_to_user(void *user_dst, const void *kernel_src, size_t len) {
    if (__builtin_expect(!user_ok((uint64_t)user_dst, len), 0)) return -EFAULT;
    int ret = 0;
    __asm__ volatile(
        "1: rep movsb\n"
        "2:\n"
        _ASM_EXTABLE(1b, 3f)
        ".pushsection .text.fixup, \"ax\"\n"
        "3: movl %[efault], %[ret]\n"
        "   jmp 2b\n"
        ".popsection\n"
        : [ret] "+r"(ret), "+D"(user_dst), "+S"(kernel_src), "+c"(len)
        : [efault] "i"(-EFAULT)
        : "memory");
    return ret;
}

/* Byte-wise user path copy with extable recovery.
 * Terminates on first NUL, returns length (excluding NUL) or
 * negative errno. Shared across syscall-entry paths. */
__attribute__((hot))
int copy_path_from_user(char *kbuf, const char *upath, size_t max) {
    if (__builtin_expect(!user_ok((uint64_t)upath, max), 0)) return -EFAULT;
    for (size_t i = 0; i < max; i++) {
        int ret = 0;
        uint8_t tmp;
        __asm__ volatile(
            "1: movb (%[src]), %[tmp]\n"
            "2:\n"
            _ASM_EXTABLE(1b, 3f)
            ".pushsection .text.fixup, \"ax\"\n"
            "3: movl %[efault], %[ret]\n"
            "   jmp 2b\n"
            ".popsection\n"
            : [ret] "+r"(ret), [tmp] "=q"(tmp)
            : [src] "r"(&upath[i]), [efault] "i"(-EFAULT)
            : "memory");
        if (__builtin_expect(ret < 0, 0)) return ret;
        kbuf[i] = (char)tmp;
        if (kbuf[i] == '\0') return i == 0 ? -ENOENT : (int)i;
    }
    return -ENAMETOOLONG;
}
