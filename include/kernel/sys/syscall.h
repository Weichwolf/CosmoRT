/* CosmoRT POSIX Syscall Layer */
#ifndef SYSCALL_H
#define SYSCALL_H

#define __KERNEL__
#include "linux/abi.h"

void fd_cleanup_entry(int fde_type, void *fde_obj);

void fd_obj_incref(int fde_type, void *fde_obj);

long sys_handler(long num, long a1, long a2, long a3, long a4, long a5, long a6);

#endif
