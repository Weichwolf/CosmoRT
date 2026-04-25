/* CosmoRT AF_UNIX Socket Layer — local bidirectional IPC */
#ifndef UNIX_SOCKET_H
#define UNIX_SOCKET_H

#include <stdint.h>

/* AF_UNIX socket states */
#define USOCK_UNUSED      0
#define USOCK_CREATED     1
#define USOCK_LISTENING   2
#define USOCK_CONNECTED   3

/* Ring buffer size per direction */
#define USOCK_BUF_SIZE    (64 * 1024)

/* Default per-listener backlog cap when listen() is called with backlog<=0
 * or not called at all (Linux: SOMAXCONN ≈ 4096, but listen(0) is treated
 * as 0 in modern Linux — we keep 0 too so the test can assert it).
 *
 * USOCK_SOMAXCONN: hard upper clamp applied inside listen() per
 * net.core.somaxconn. Linux default is 4096; a per-listener queue still
 * grows on demand via slab so this is just a sanity cap. */
#define USOCK_SOMAXCONN 4096

/* sockaddr_un (matches Linux) */
struct k_sockaddr_un {
    uint16_t sun_family;
    char     sun_path[108];
};

/* Forward decl */
typedef struct unix_socket unix_socket_t;

struct unix_socket {
    int       state;
    int       refcount;
    unix_socket_t *peer;              /* connected partner */

    /* Per-socket receive buffer (peer writes here) */
    uint8_t   buf[USOCK_BUF_SIZE];
    int       head, tail, count;

    /* Bound path. path_len=0: unbound. path_len>0: bound.
     * Abstract namespace: path[0]==0 with path_len>0 (Linux: paths
     * starting with NUL are abstract — not on the filesystem). */
    char      path[108];
    int       path_len;

    /* Accept queue (for LISTENING sockets) — singly-linked FIFO of pending
     * client sockets. Each client links via its own backlog_next pointer
     * (only one backlog hop per pending socket — owner is unique). The
     * cap is per-listener via listen(fd, n); no systemwide pool. */
    unix_socket_t *backlog_head;   /* oldest pending — accept() dequeues here */
    unix_socket_t *backlog_tail;   /* newest pending — connect() appends here */
    int            backlog_count;
    int            backlog_cap;    /* set by listen(); 0 = "not listening yet" */
    unix_socket_t *backlog_next;   /* link inside another listener's queue */
    unix_socket_t *backlog_owner;  /* listener whose queue we sit in (or NULL) */

    /* Flags from socket() */
    int       flags;

    /* MSG_OOB state (AF_UNIX SOCK_STREAM): genau 1 Byte OOB-Puffer pro
     * Empfaenger. Linux haelt das im oob_skb (SKB); wir serialisieren nur.
     * oob_present=1: oob_byte gueltig; wird von recv(MSG_OOB) geholt. */
    uint8_t   oob_byte;
    int       oob_present;

    /* Blocked reader (waiting for data) */
    void     *blocked_reader;   /* thread_t* — blocked in read() */
    /* Blocked accept (waiting for incoming connection) */
    void     *blocked_acceptor; /* thread_t* — blocked in accept() */

    /* Owning network namespace. Abstract sockets (path[0]==0) are isolated
     * per-NS; pathname sockets remain global because they live on the VFS,
     * which is not namespaced in Phase 15. */
    uint32_t  ns_id;

    /* Global active list (intrusive) — replaces static pool scan for
     * bind-name-collision/connect-path-lookup. */
    unix_socket_t *next_active;
    unix_socket_t *prev_active;
};

/* Syscall implementations */
long usock_socket(int type);
long usock_socketpair(int type, int *sv);
long usock_bind(int fd, const struct k_sockaddr_un *addr, int addrlen);
long usock_listen(int fd, int backlog);
long usock_accept4(int fd, void *addr, int *addrlen, int flags);
long usock_connect(int fd, const struct k_sockaddr_un *addr, int addrlen);
long usock_sendmsg(int fd, const void *msghdr, int flags);
long usock_recvmsg(int fd, void *msghdr, int flags);
long usock_send(int fd, const void *buf, long len, int flags);
long usock_recv(int fd, void *buf, long len, int flags);

/* FD ops (called from do_read/do_write/do_close) */
long usock_read(int fd, void *buf, long count);
long usock_read_blocking(unix_socket_t *s, void *buf, long count);
long usock_write(int fd, const void *buf, long count);
long usock_close(int fd);

/* Refcount management */
void usock_incref(void *obj);
void usock_decref(void *obj);

/* Get unix_socket from fd (for poll) */
unix_socket_t *usock_from_fd(int fd);

#endif
