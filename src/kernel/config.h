/* CosmoRT Kernel Configuration */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

/* Higher-half direct physical map: PML4[256..263] → first 8GB */
#define PHYS_OFFSET    0xFFFF800000000000ULL

#define phys_to_virt(p) ((void *)((uint64_t)(p) + PHYS_OFFSET))
#define virt_to_phys(v) ((uint64_t)(v) - PHYS_OFFSET)

/* Ensure address is in direct map (handles EFI-relocated identity-mapped symbols).
 * Identity-mapped addrs are < 8GB; direct-map addrs are >= PHYS_OFFSET. */
static inline uint64_t ensure_high(uint64_t addr) {
    return (addr < PHYS_OFFSET) ? addr + PHYS_OFFSET : addr;
}

/* Process */
#define PROC_MAX         16
#define KSTACK_SIZE      (64 * 1024)

/* SMP */
#define SMP_MAX_CORES    64
#define SMP_STACK_SIZE   65536
#define SMP_AP_TIMEOUT_MS 500

/* IPC */
#define IPC_MAX_ENDPOINTS 64

/* Periodic */
#define PERIODIC_TICKS   50

/* User virtual address layout */
#define USER_STACK_TOP   0x7FFFFFFFE000ULL
#define USER_STACK_SIZE  (8 * 1024 * 1024)  /* 8MB */
#define USER_MMAP_BASE   0x7F0000000000ULL
#define USER_BRK_BASE    0x600000ULL         /* above typical ELF load */

#endif
