/* CosmoRT Socket Layer — bridges FD table to net_tcp_* */
#ifndef SOCKET_H
#define SOCKET_H

#include "net/net.h"
#include "mm/slab.h"

/* Socket state */
#define SOCK_UNUSED    0
#define SOCK_CREATED   1
#define SOCK_CONNECTED 2
#define SOCK_LISTENING 3
#define SOCK_SHUTDOWN  4

/* Socket option flags (bitfield) */
#define SOCKF_REUSEADDR  (1 << 0)
#define SOCKF_KEEPALIVE  (1 << 1)
#define SOCKF_NODELAY    (1 << 2)
#define SOCKF_CONNECTING (1 << 3) /* non-blocking connect in progress */

typedef struct socket socket_t;

struct socket {
    net_tcp_t tcp;
    int       state;
    int       refcount;    /* number of FDs referencing this socket */

    /* IPv6 support */
    uint8_t   is_v6;       /* 1 = AF_INET6 socket */
    uint8_t   v6only;      /* IPV6_V6ONLY default 1 */
    uint8_t   _pad6[2];

    /* bind state */
    uint32_t  local_ip;    /* big-endian (IPv4) */
    uint16_t  local_port;  /* big-endian */
    struct in6_addr local_ip6;   /* IPv6 bind addr */

    /* connect/accept state */
    uint32_t  remote_ip;   /* big-endian (IPv4) */
    uint16_t  remote_port; /* big-endian */
    struct in6_addr remote_ip6;  /* IPv6 connect addr */

    /* shutdown flags */
    uint8_t   shut_rd;
    uint8_t   shut_wr;

    /* UDP (SOCK_DGRAM) */
    uint8_t   is_dgram;
    uint16_t  udp_local_port; /* host byte order, for net_poll matching */

    /* socket options */
    uint32_t  sockflags;

    /* Owning network-namespace ID. Captured at socket() time, freed when
     * the socket is released. Listener lookup keys on (port, ns_id) so
     * two NS can hold the same port concurrently. */
    uint32_t  ns_id;

    /* Actual accept path runs through net_tcp_accept (tcp.c); no in-socket
     * queue needed. listen()-backlog is tracked there. */

    /* Recv deadline across syscall restarts (0 = not set) */
    uint64_t     recv_deadline;

    /* Active-list linkage (intrusive doubly-linked) */
    socket_t     *next_active;
    socket_t     *prev_active;
};

/* Syscall implementations */
long do_socket(int domain, int type, int protocol);
long do_connect(int fd, const void *addr, int addrlen);
long do_bind(int fd, const void *addr, int addrlen);
long do_listen(int fd, int backlog);
long do_accept4(int fd, void *addr, int *addrlen, int acc_flags);
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

/* Slab pool + active list (defined in socket.c) */
extern slab_t sock_slab;
extern socket_t *sock_active_head;

/* Alloc/free (O(1) via slab + active list) */
socket_t *sock_alloc(void);
void sock_free(socket_t *s);

/* Returns 1 if any socket is in SOCK_LISTENING state on local_port in the
 * given NS, else 0. Used by tcp_input to decide RST-on-closed-port. */
int sock_has_listener(uint32_t ns_id, uint16_t local_port_host);

/* Returns the listening socket for (ns_id, local_port_host), or NULL. */
socket_t *sock_find_listener(uint32_t ns_id, uint16_t local_port_host);

/* IPv6-only variant — matches a listening AF_INET6 socket.
 * If a v6 listener has v6only=0 (dual-stack) it accepts v4 too via the
 * v4 path (Linux semantic). */
socket_t *sock_find_listener6(uint32_t ns_id, uint16_t local_port_host);

/* Called from do_read/do_write/do_close for FD_SOCKET */
long socket_read(int fd, void *buf, long count);
long socket_write(int fd, const void *buf, long count);
long socket_close(int fd);

/* AF_UNIX sockets */
#include "net/unix_socket.h"

#endif
