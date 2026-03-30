/* CosmoRT AF_UNIX Socket Layer — local bidirectional IPC */
#ifndef UNIX_SOCKET_H
#define UNIX_SOCKET_H

#include <stdint.h>

#define USOCK_UNUSED      0
#define USOCK_CREATED     1
#define USOCK_LISTENING   2
#define USOCK_CONNECTED   3

#define USOCK_BUF_SIZE    (64 * 1024)

#define USOCK_MAX         32

#define USOCK_BACKLOG_MAX 8

struct k_sockaddr_un {
    uint16_t sun_family;
    char     sun_path[108];
};

typedef struct unix_socket unix_socket_t;

struct unix_socket {
    int       state;
    int       refcount;
    unix_socket_t *peer;

    uint8_t   buf[USOCK_BUF_SIZE];
    int       head, tail, count;

    char      path[108];

    unix_socket_t *backlog[USOCK_BACKLOG_MAX];
    int       backlog_count;

    int       flags;

    void     *blocked_reader;
};

long usock_socket(int type);
long usock_socketpair(int type, int *sv);
long usock_bind(int fd, const struct k_sockaddr_un *addr, int addrlen);
long usock_listen(int fd, int backlog);
long usock_accept4(int fd, void *addr, int *addrlen, int flags);
long usock_connect(int fd, const struct k_sockaddr_un *addr, int addrlen);
long usock_sendmsg(int fd, const void *msghdr, int flags);
long usock_recvmsg(int fd, void *msghdr, int flags);

long usock_read(int fd, void *buf, long count);
long usock_read_blocking(unix_socket_t *s, void *buf, long count);
long usock_write(int fd, const void *buf, long count);
long usock_close(int fd);

void usock_incref(void *obj);
void usock_decref(void *obj);

unix_socket_t *usock_from_fd(int fd);

#endif
