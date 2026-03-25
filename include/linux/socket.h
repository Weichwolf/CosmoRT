/* Linux x86_64 ABI — socket constants */
#ifndef COSMO_LINUX_SOCKET_H
#define COSMO_LINUX_SOCKET_H

#include "types.h"

#define AF_UNIX         1
#define AF_INET         2
#define AF_INET6        10
#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_KEEPALIVE    9
#define IPPROTO_TCP     6
#define TCP_NODELAY     1

#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

#endif /* COSMO_LINUX_SOCKET_H */
