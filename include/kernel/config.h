/* CosmoRT Kernel Configuration */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

/* Higher-half direct physical map — canonical source is cosmo.h */
#include "cosmort.h"
#define PHYS_OFFSET    COSMO_PHYS_OFFSET

/* Physical ↔ Virtual address translation (kernel-internal) */
#ifndef phys_to_virt
#define phys_to_virt(p) ((void *)((uint64_t)(p) + PHYS_OFFSET))
#endif
#ifndef virt_to_phys
#define virt_to_phys(v) ((uint64_t)(v) - PHYS_OFFSET)
#endif

/* Ensure address is in direct map (handles EFI-relocated identity-mapped symbols).
 * Identity-mapped addrs are < 8GB; direct-map addrs are >= PHYS_OFFSET. */
static inline uint64_t ensure_high(uint64_t addr) {
    return (addr < PHYS_OFFSET) ? addr + PHYS_OFFSET : addr;
}

/* Process */
#define KSTACK_SIZE      (64 * 1024)

/* XSAVE area: max size for x87+SSE+AVX+AVX-512.
 * Actual size determined at boot via CPUID.0DH. Fallback = 512 (FXSAVE). */
#define XSAVE_MAX_SIZE   2688

/* SMP (single-core for POSIX phase) */
#define SMP_MAX_CORES    1

/* IPC */
#define IPC_MAX_ENDPOINTS 64

/* Periodic */
#define PERIODIC_TICKS   50

/* User virtual address layout */
#define USER_STACK_TOP   0x7FFFFFFFE000ULL
#define USER_STACK_INIT  (132 * 1024)       /* initial stack VMA (132KB, like Linux) */
#define RLIM_STACK_DEFAULT (8UL * 1024 * 1024) /* default RLIMIT_STACK: 8MB */
#define RLIM_STACK_MAX     (64UL * 1024 * 1024) /* hard limit: 64MB */
#define STACK_GUARD_GAP  (1UL * 1024 * 1024)    /* 1MB gap between stack and adjacent VMAs */
#define USER_MMAP_BASE   0x7F0000000000ULL
#define USER_BRK_BASE    0x600000ULL         /* above typical ELF load */

/* Network */
#define NET_PKT_SIZE       1536    /* max packet size (MTU + headers) */
#define NET_QUEUE_SIZE     128     /* packets per queue */
#define NET_TCP_RXBUF      65536   /* TCP receive buffer per socket (power of 2, allocated on connect) */
#define NET_TCP_TIMEOUT_MS 30000   /* TCP connect/recv timeout (TLS handshake needs time) */
#define NET_DHCP_RETRY_MS  3000    /* DHCP retry interval */
#define NET_MAX_SOCKETS    256     /* slab capacity for inet sockets */
#define NET_TCP_INIT_RTO_MS 1000   /* initial retransmit timeout */
#define NET_TCP_MAX_RTO_MS  60000  /* max retransmit timeout */
#define NET_TCP_KEEPALIVE_INTERVAL_MS 75000  /* keepalive probe interval (75s) */
#define NET_TCP_KEEPALIVE_MAX_PROBES  9      /* probes before connection dead */

#endif
