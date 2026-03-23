/* CosmoRT POSIX Syscall Layer
 *
 * All constants (SYS_*, errno, flags) live in cosmo_uapi.h.
 * This header re-exports them and declares kernel-internal syscall functions.
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#define __KERNEL__
#include "cosmo_uapi.h"

/* Clean up a single FD entry (socket, pipe, etc.) during process exit.
 * Does NOT free FD_FILE — caller handles that via vfs.
 * fde_type: FD type, fde_obj: kernel object pointer. */
void fd_cleanup_entry(int fde_type, void *fde_obj);

/* Increment refcount on a non-file FD object (for fork/dup) */
void fd_obj_incref(int fde_type, void *fde_obj);

/* Main syscall dispatcher — called from ASM entry and INT 0x80 */
long sys_handler(long num, long a1, long a2, long a3, long a4, long a5, long a6);

#endif
