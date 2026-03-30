/* CosmoRT IP — Header build, checksum, send. Extracted from net.c (Phase C) */
#ifndef IP_H
#define IP_H

#include <stdint.h>

void ip_build_header(uint8_t *pkt, const uint8_t *dst_mac,
                     const uint8_t *dst_ip, uint8_t proto, uint16_t plen);

void ip_build_header_tos(uint8_t *pkt, const uint8_t *dst_mac,
                         const uint8_t *dst_ip, uint8_t proto,
                         uint16_t plen, uint8_t tos);

void ip_send_raw(const uint8_t *data, uint16_t len);

#endif
