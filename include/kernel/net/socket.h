/* CosmoRT Socket Layer — bridges FD table to net_tcp_* */
#ifndef SOCKET_H
#define SOCKET_H

#include "net/net.h"
#include "mm/slab.h"

#define SOCK_SLAB_CAP NET_MAX_SOCKETS

#define SOCK_UNUSED    0
#define SOCK_CREATED   1
#define SOCK_CONNECTED 2
#define SOCK_LISTENING 3
#define SOCK_SHUTDOWN  4

#define ACCEPT_QUEUE_MAX 8

#define SOCKF_REUSEADDR  (1 << 0)
#define SOCKF_KEEPALIVE  (1 << 1)
#define SOCKF_NODELAY    (1 << 2)
#define SOCKF_CONNECTING (1 << 3)

typedef struct socket socket_t;

typedef struct {
    net_tcp_t tcp;
    uint32_t  remote_ip;
    uint16_t  remote_port;
    uint16_t  _pad;
} accept_conn_t;

struct socket {
    net_tcp_t tcp;
    int       state;
    int       refcount;

    uint32_t  local_ip;
    uint16_t  local_port;

    uint32_t  remote_ip;
    uint16_t  remote_port;

    uint8_t   shut_rd;
    uint8_t   shut_wr;

    uint8_t   is_dgram;
    uint16_t  udp_local_port;

    uint32_t  sockflags;

    accept_conn_t accept_q[ACCEPT_QUEUE_MAX];
    int           accept_head;
    int           accept_count;

    uint64_t     recv_deadline;

    socket_t     *next_active;
    socket_t     *prev_active;
};

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

extern slab_t sock_slab;
extern socket_t *sock_active_head;

socket_t *sock_alloc(void);
void sock_free(socket_t *s);

long socket_read(int fd, void *buf, long count);
long socket_write(int fd, const void *buf, long count);
long socket_close(int fd);

#include "net/unix_socket.h"

#endif
