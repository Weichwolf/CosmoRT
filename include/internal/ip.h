/* CosmoRT IP — Header build, checksum, send. Extracted from net.c (Phase C) */
#ifndef IP_H
#define IP_H

#include <stdint.h>

/* Build Ethernet+IP header (14 + 20 bytes) at pkt.
 * proto: IP protocol (6=TCP, 17=UDP, 1=ICMP).
 * plen: payload length after IP header. */
void ip_build_header(uint8_t *pkt, const uint8_t *dst_mac,
                     const uint8_t *dst_ip, uint8_t proto, uint16_t plen);

/* Send raw Ethernet frame via NIC (or loopback). */
void ip_send_raw(const uint8_t *data, uint16_t len);

#endif
