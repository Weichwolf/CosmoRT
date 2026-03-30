/* CosmoRT kexec — kernel hot-swap without full reboot */
#ifndef KEXEC_H
#define KEXEC_H

#include <stddef.h>

int do_kexec(const void *image, size_t len);

#endif
