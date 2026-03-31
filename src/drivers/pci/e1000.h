/* Intel E1000 NIC Driver — adapted for CosmoRT higher-half kernel
 * PCI device 8086:100e (82540EM), uses cosmo_* hw primitives.
 */
#ifndef E1000_H
#define E1000_H

#include <stdint.h>

/* Initialize E1000: PCI scan via cosmo_pci_config_read, MMIO via cosmo_mmio_map.
 * Returns 0 on success, -1 if no E1000 found. */
int e1000_init(void);

/* Get MAC address */
void e1000_get_mac(uint8_t mac[6]);

/* Send raw Ethernet frame */
int e1000_send(const void *data, uint16_t len);

/* Receive raw Ethernet frame. Returns length, 0 if none. */
int e1000_recv(void *buf, uint16_t bufsize);

#endif
