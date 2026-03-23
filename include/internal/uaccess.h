/* User-space access helpers — copy_from_user / copy_to_user
 *
 * Validates user pointer range before copying.
 * Returns 0 on success, -EFAULT on bad address. */
#ifndef UACCESS_H
#define UACCESS_H

#include <stdint.h>
#include <stddef.h>
#include "memops.h"
#include "linux.h"

/* Validate user pointer: must be in lower half, no overflow */
static inline int user_ok(uint64_t addr, size_t len) {
    return addr < 0x800000000000ULL &&
           addr + len <= 0x800000000000ULL &&
           addr + len >= addr;
}

static inline int copy_from_user(void *kernel_dst, const void *user_src, size_t len) {
    if (__builtin_expect(!user_ok((uint64_t)user_src, len), 0)) return -EFAULT;
    kmemcpy(kernel_dst, user_src, len);
    return 0;
}

static inline int copy_to_user(void *user_dst, const void *kernel_src, size_t len) {
    if (__builtin_expect(!user_ok((uint64_t)user_dst, len), 0)) return -EFAULT;
    kmemcpy(user_dst, kernel_src, len);
    return 0;
}

#endif
