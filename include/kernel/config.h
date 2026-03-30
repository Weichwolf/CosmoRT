/* CosmoRT Kernel Configuration */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

#include "cosmort.h"
#define PHYS_OFFSET    COSMO_PHYS_OFFSET

static inline uint64_t ensure_high(uint64_t addr) {
    return (addr < PHYS_OFFSET) ? addr + PHYS_OFFSET : addr;
}

#define KSTACK_SIZE      (64 * 1024)

#define SMP_MAX_CORES    64
#define SMP_STACK_SIZE   65536
#define SMP_AP_TIMEOUT_MS 500

#define IPC_MAX_ENDPOINTS 64

#define PERIODIC_TICKS   50

#define USER_STACK_TOP   0x7FFFFFFFE000ULL
#define USER_STACK_SIZE  (8 * 1024 * 1024)
#define USER_MMAP_BASE   0x7F0000000000ULL
#define USER_BRK_BASE    0x600000ULL

#define NET_PKT_SIZE       1536
#define NET_QUEUE_SIZE     128
#define NET_TCP_RXBUF      65536
#define NET_TCP_TIMEOUT_MS 30000
#define NET_DHCP_RETRY_MS  3000
#define NET_MAX_SOCKETS    256
#define NET_TCP_INIT_RTO_MS 1000
#define NET_TCP_MAX_RTO_MS  60000
#define NET_TX_RING_SIZE   131072
#define NET_TCP_KEEPALIVE_INTERVAL_MS 75000
#define NET_TCP_KEEPALIVE_MAX_PROBES  9

#endif
