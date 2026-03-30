/* Kernel<->Driver shared ring port for userspace NIC drivers. */

#ifndef NET_PORT_H
#define NET_PORT_H

#include <stdint.h>
#include <stddef.h>
#include "ring.h"

#define NET_PORT_RING_SIZE  (64 * 1024)

struct net_port {
    ring_t  *tx_ring;
    ring_t  *rx_ring;
    void    *shm_virt;
    uint64_t shm_phys;
    size_t   shm_size;
    uint8_t  mac[6];
    int      driver_pid;
    int      active;
};

void net_port_init(void);

int net_port_attach(uint64_t shm_phys, size_t shm_size, const uint8_t mac[6]);

void net_port_detach(void);

int net_port_active(void);

void net_port_check_driver(int pid);

#endif
