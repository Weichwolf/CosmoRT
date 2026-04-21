/* Linux x86_64 ABI — socket constants */
#ifndef COSMO_LINUX_SOCKET_H
#define COSMO_LINUX_SOCKET_H

#include "types.h"

#define AF_UNIX         1
#define AF_INET         2
#define AF_INET6        10
#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_SEQPACKET  5
#define SOL_IP          0
#define SOL_SOCKET      1

/* IP/Multicast options (SOL_IP / IPPROTO_IP) */
#define SOCKOPT_IP_ADD_MEMBERSHIP   35
#define SOCKOPT_IP_DROP_MEMBERSHIP  36
#define SOCKOPT_MCAST_JOIN_GROUP    42
#define SOCKOPT_MCAST_LEAVE_GROUP   45

/* Well-known/privileged port ceiling (BSD IPPORT_RESERVED) — bind <this requires
 * euid==root (CAP_NET_BIND_SERVICE in Linux). */
#define SOCKET_PRIVILEGED_PORT_MAX  1024
#define SO_REUSEADDR    2
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_OOBINLINE    10
#define SO_LINGER       13
#define SO_REUSEPORT    15
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_ERROR        4
#define IPPROTO_TCP     6
#define TCP_NODELAY     1
#define TCP_CORK        3
#define TCP_KEEPIDLE    4
#define TCP_KEEPINTVL   5
#define TCP_KEEPCNT     6
#define TCP_QUICKACK    12
#define TCP_FASTOPEN    23
#define TCP_FASTOPEN_CONNECT 30

#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

#endif /* COSMO_LINUX_SOCKET_H */
