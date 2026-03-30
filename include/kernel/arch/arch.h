/* CosmoRT Architecture Abstraction */
#ifndef COSMO_ARCH_H
#define COSMO_ARCH_H

#if defined(__x86_64__)
#include "arch/x86_64.h"
#elif defined(__aarch64__)
#include "arch/aarch64.h"
#elif defined(__riscv) && __riscv_xlen == 64
#include "arch/riscv64.h"
#else
#error "Unsupported architecture"
#endif

#endif
