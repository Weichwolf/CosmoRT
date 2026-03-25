/* CosmoRT Socket Layer — bridges FD table to net_tcp_* */
#ifndef SOCKET_H
#define SOCKET_H

#include "net.h"

#define MAX_SOCKETS NET_MAX_SOCKETS

/* Socket state */
#define SOCK_UNUSED    0
#define SOCK_CREATED   1
#define SOCK_CONNECTED 2
#define SOCK_LISTENING 3
#define SOCK_SHUTDOWN  4

/* Accept queue capacity */
#define ACCEPT_QUEUE_MAX 8

/* Socket option flags (bitfield) */
#define SOCKF_REUSEADDR  (1 << 0)
#define SOCKF_KEEPALIVE  (1 << 1)
#define SOCKF_NODELAY    (1 << 2)

typedef struct socket socket_t;

/* Pending connection in accept queue */
typedef struct {
    net_tcp_t tcp;
    uint32_t  remote_ip;   /* big-endian */
    uint16_t  remote_port; /* big-endian */
    uint16_t  _pad;
} accept_conn_t;

struct socket {
    net_tcp_t tcp;
    int       state;
    int       refcount;    /* number of FDs referencing this socket */

    /* bind state */
    uint32_t  local_ip;    /* big-endian */
    uint16_t  local_port;  /* big-endian */

    /* connect/accept state */
    uint32_t  remote_ip;   /* big-endian */
    uint16_t  remote_port; /* big-endian */

    /* shutdown flags */
    uint8_t   shut_rd;
    uint8_t   shut_wr;

    /* UDP (SOCK_DGRAM) */
    uint8_t   is_dgram;
    uint16_t  udp_local_port; /* host byte order, for net_poll matching */

    /* socket options */
    uint32_t  sockflags;

    /* accept queue (only for listening sockets) */
    accept_conn_t accept_q[ACCEPT_QUEUE_MAX];
    int           accept_head;
    int           accept_count;
};

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
long do_shutdown(int fd, int how);
long do_poll(void *fds, int nfds, int timeout);

/* Called from do_read/do_write/do_close for FD_SOCKET */
long socket_read(int fd, void *buf, long count);
long socket_write(int fd, const void *buf, long count);
long socket_close(int fd);

/* AF_UNIX sockets */
#include "unix_socket.h"

#endif
