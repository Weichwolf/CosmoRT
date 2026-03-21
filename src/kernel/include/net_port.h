/* Kernel<->Driver shared ring port for userspace NIC drivers.
 * BeOS-inspired: shared memory areas with lock-free ring buffers.
 * Kernel writes TX ring, reads RX ring. Driver does the opposite. */

#ifndef NET_PORT_H
#define NET_PORT_H

#include <stdint.h>
#include <stddef.h>
#include "ring.h"

/* Shared memory layout:
 *   [0, RING_SIZE)            TX ring (kernel->driver: packets to send)
 *   [RING_SIZE, 2*RING_SIZE)  RX ring (driver->kernel: received packets)
 *
 * Each ring entry is length-prefixed: [uint16_t len][payload bytes] */
#define NET_PORT_RING_SIZE  (64 * 1024)

struct net_port {
    ring_t  *tx_ring;      /* kernel writes, driver reads */
    ring_t  *rx_ring;      /* driver writes, kernel reads */
    void    *shm_virt;     /* kernel virtual addr of shared memory */
    uint64_t shm_phys;     /* physical addr (for driver mapping) */
    size_t   shm_size;
    uint8_t  mac[6];
    int      driver_pid;   /* for crash detection */
    int      active;
};

/* Initialize net_port subsystem */
void net_port_init(void);

/* Driver registers itself via SYS_COSMO_NIC_ATTACH.
 * shm_phys: physical address of shared memory (from cosmo_dma_alloc)
 * shm_size: total size (must be >= 2 * NET_PORT_RING_SIZE)
 * mac: 6-byte MAC address */
int net_port_attach(uint64_t shm_phys, size_t shm_size, const uint8_t mac[6]);

/* Detach current driver */
void net_port_detach(void);

/* Check if a port-based driver is active */
int net_port_active(void);

/* Called from do_exit — detach if exiting process is the driver */
void net_port_check_driver(int pid);

#endif
