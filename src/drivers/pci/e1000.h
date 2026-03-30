/* Intel E1000 NIC Driver — adapted for CosmoRT higher-half kernel */
#ifndef E1000_H
#define E1000_H

#include <stdint.h>

int e1000_init(void);

void e1000_get_mac(uint8_t mac[6]);

int e1000_send(const void *data, uint16_t len);

int e1000_recv(void *buf, uint16_t bufsize);

#endif
