/* CosmoRT CSPRNG — ChaCha20-based, works without RDRAND */
#ifndef RANDOM_H
#define RANDOM_H

#include <stddef.h>
#include "boot_info.h"

void random_init(struct boot_info *info);
int  random_get(void *buf, size_t len);
void random_add_interrupt_entropy(void);

#endif
