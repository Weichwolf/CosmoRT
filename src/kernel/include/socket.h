/* CosmoRT Socket Layer — bridges FD table to net_tcp_* */
#ifndef SOCKET_H
#define SOCKET_H

#include "net.h"

#define MAX_SOCKETS 16

/* Socket state */
#define SOCK_UNUSED    0
#define SOCK_CREATED   1
#define SOCK_CONNECTED 2

typedef struct {
    net_tcp_t tcp;
    int       state;
    int       refcount;   /* number of FDs referencing this socket */
} socket_t;

/* Syscall implementations */
long do_socket(int domain, int type, int protocol);
long do_connect(int fd, const void *addr, int addrlen);
long do_bind(int fd, const void *addr, int addrlen);
long do_listen(int fd, int backlog);
long do_accept(int fd, void *addr, int *addrlen);
long do_sendto(int fd, const void *buf, long len, int flags,
               const void *dest_addr, int addrlen);
long do_recvfrom(int fd, void *buf, long len, int flags,
                 void *src_addr, int *addrlen);
long do_setsockopt(int fd, int level, int optname, const void *optval, int optlen);
long do_getsockopt(int fd, int level, int optname, void *optval, int *optlen);
long do_getsockname(int fd, void *addr, int *addrlen);
long do_getpeername(int fd, void *addr, int *addrlen);
long do_poll(void *fds, int nfds, int timeout);

/* Called from do_read/do_write/do_close for FD_SOCKET */
long socket_read(int fd, void *buf, long count);
long socket_write(int fd, const void *buf, long count);
long socket_close(int fd);

#endif
