/* CosmoRT kexec — kernel hot-swap without full reboot */
#ifndef KEXEC_H
#define KEXEC_H

#include <stddef.h>

/* Load a new kernel ELF image and jump to it.
 * Stops all processes, flushes filesystem, resets APs,
 * copies new kernel to physical memory, jumps to entry.
 * Does not return on success. Returns -errno on failure. */
int do_kexec(const void *image, size_t len);

#endif
