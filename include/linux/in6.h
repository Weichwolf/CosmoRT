/* Linux x86_64 ABI — IPv6 address + sockaddr_in6 (RFC 3493) */
#ifndef COSMO_LINUX_IN6_H
#define COSMO_LINUX_IN6_H

#include "types.h"

/* IPv6 address — 128 bits. Linux exposes overlapping octet/word/dword views
 * via union; we mirror that so user-supplied sockaddrs cast cleanly. */
struct in6_addr {
    union {
        uint8_t  u6_addr8[16];
        uint16_t u6_addr16[8];
        uint32_t u6_addr32[4];
    } in6_u;
};

/* Linux short-name aliases (musl uses these). */
#define s6_addr     in6_u.u6_addr8
#define s6_addr16   in6_u.u6_addr16
#define s6_addr32   in6_u.u6_addr32

/* sockaddr_in6 — 28 bytes total, packed exactly like Linux. */
struct sockaddr_in6 {
    uint16_t        sin6_family;        /* AF_INET6 = 10 */
    uint16_t        sin6_port;          /* big-endian */
    uint32_t        sin6_flowinfo;      /* big-endian, RFC 6437 */
    struct in6_addr sin6_addr;
    uint32_t        sin6_scope_id;      /* link-local interface index */
};

/* Well-known addresses */
#define IN6ADDR_ANY_INIT      { { { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 } } }
#define IN6ADDR_LOOPBACK_INIT { { { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 } } }

/* IPv6 protocol numbers (Next Header values) */
#define IPPROTO_HOPOPTS     0    /* Hop-by-Hop options */
#define IPPROTO_TCP_V6      6
#define IPPROTO_UDP_V6      17
#define IPPROTO_ROUTING     43
#define IPPROTO_FRAGMENT    44
#define IPPROTO_ICMPV6      58
#define IPPROTO_NONE        59
#define IPPROTO_DSTOPTS     60

/* Socket-level options (RFC 3493) */
#define IPPROTO_IPV6        41
#define IPV6_ADDRFORM       1
#define IPV6_V6ONLY         26
#define IPV6_UNICAST_HOPS   16
#define IPV6_MULTICAST_HOPS 18

/* ICMPv6 message types (RFC 4443 + RFC 4861) */
#define ICMPV6_DEST_UNREACH       1
#define ICMPV6_PACKET_TOO_BIG     2
#define ICMPV6_TIME_EXCEEDED      3
#define ICMPV6_PARAM_PROBLEM      4
#define ICMPV6_ECHO_REQUEST       128
#define ICMPV6_ECHO_REPLY         129
#define ICMPV6_RT_SOLICIT         133
#define ICMPV6_RT_ADVERT          134
#define ICMPV6_NB_SOLICIT         135
#define ICMPV6_NB_ADVERT          136

#endif /* COSMO_LINUX_IN6_H */
