/* User-space access helpers — extable-based fault recovery.
 *
 * Each primitive wraps a single inline-asm user-memory touch annotated with
 * _ASM_EXTABLE. On page fault the handler rewrites RIP to the fixup label
 * which returns -EFAULT. No thread state, no jmpbuf — pure RIP rewrite.
 *
 * SMAP: the AC flag is set by syscall_entry (STAC) and cleared on return
 * (CLAC). These helpers run within syscall context where user access is
 * already permitted. */
#ifndef UACCESS_H
#define UACCESS_H

#include <stdint.h>
#include <stddef.h>
#include "linux/abi.h"

/* Validate user pointer: must be in lower half, no overflow. */
static inline int user_ok(uint64_t addr, size_t len) {
    return addr < 0x800000000000ULL &&
           addr + len <= 0x800000000000ULL &&
           addr + len >= addr;
}

/* Copy LEN bytes from user to kernel. Returns 0 on success, -EFAULT on fault. */
int copy_from_user(void *kernel_dst, const void *user_src, size_t len);

/* Copy LEN bytes from kernel to user. Returns 0 on success, -EFAULT on fault. */
int copy_to_user(void *user_dst, const void *kernel_src, size_t len);

/* Copy user NUL-terminated path string to kernel buffer with bounds checks.
 * Returns length excluding NUL, -ENOENT for empty string, -ENAMETOOLONG,
 * or -EFAULT. */
int copy_path_from_user(char *kbuf, const char *upath, size_t max);

#endif
