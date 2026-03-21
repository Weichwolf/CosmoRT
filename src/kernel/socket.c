/* CosmoRT Socket Layer — maps POSIX socket syscalls to net_tcp_* */

#include "socket.h"
#include "process.h"
#include "fd.h"
#include "serial.h"
#include "timer.h"
#include "syscall.h"
#include "spinlock.h"

/* sockaddr_in layout (user-space struct, 16 bytes) */
struct k_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;     /* big-endian */
    uint32_t sin_addr;     /* big-endian */
    uint8_t  sin_zero[8];
};

/* Validate user pointer: must be in lower half */
static inline int user_ok(uint64_t addr, size_t len) {
    return addr < 0x800000000000ULL &&
           addr + len <= 0x800000000000ULL &&
           addr + len >= addr;
}

/* Socket pool */
static socket_t sockets[MAX_SOCKETS];
static spinlock_t sock_lock = SPINLOCK_INIT;

static socket_t *sock_alloc(void) {
    uint64_t flags;
    spin_lock_irq(&sock_lock, &flags);
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].state == SOCK_UNUSED) {
            sockets[i].state = SOCK_CREATED;
            spin_unlock_irq(&sock_lock, flags);
            return &sockets[i];
        }
    }
    spin_unlock_irq(&sock_lock, flags);
    return 0;
}

static socket_t *sock_from_fd(int fd) {
    process_t *p = proc_current();
    if (!p) return 0;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_SOCKET) return 0;
    return (socket_t *)fde->obj;
}

/* ── SYS_SOCKET (41) ─────────────────────────────── */

long do_socket(int domain, int type, int protocol) {
    (void)domain; (void)type; (void)protocol;
    /* Only AF_INET + SOCK_STREAM supported */
    socket_t *s = sock_alloc();
    if (!s) return -EMFILE;

    process_t *p = proc_current();
    if (!p) { s->state = SOCK_UNUSED; return -EFAULT; }

    int fd = fd_alloc(&p->fds, FD_SOCKET, s, O_RDWR);
    if (fd < 0) { s->state = SOCK_UNUSED; return -EMFILE; }
    return fd;
}

/* ── SYS_CONNECT (42) ────────────────────────────── */

long do_connect(int fd, const void *addr, int addrlen) {
    if (addrlen < (int)sizeof(struct k_sockaddr_in)) return -EINVAL;
    if (!user_ok((uint64_t)addr, (size_t)addrlen)) return -EFAULT;

    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state == SOCK_CONNECTED) return -EINVAL;

    const struct k_sockaddr_in *sa = (const struct k_sockaddr_in *)addr;
    uint32_t ip_be = sa->sin_addr;
    uint8_t dst_ip[4] = {
        (uint8_t)(ip_be & 0xFF),
        (uint8_t)((ip_be >> 8) & 0xFF),
        (uint8_t)((ip_be >> 16) & 0xFF),
        (uint8_t)((ip_be >> 24) & 0xFF)
    };
    /* sin_port is big-endian, net_tcp_connect expects host uint16_t */
    uint16_t port = (uint16_t)((sa->sin_port >> 8) | (sa->sin_port << 8));

    /* Zero the tcp struct */
    for (int i = 0; i < (int)sizeof(net_tcp_t); i++)
        ((uint8_t *)&s->tcp)[i] = 0;

    if (net_tcp_connect(&s->tcp, dst_ip, port) < 0) return -ETIMEDOUT;
    s->state = SOCK_CONNECTED;
    return 0;
}

/* ── SYS_SENDTO (44) ─────────────────────────────── */

long do_sendto(int fd, const void *buf, long len, int flags,
               const void *dest_addr, int addrlen) {
    (void)flags; (void)dest_addr; (void)addrlen;
    if (!user_ok((uint64_t)buf, (size_t)len)) return -EFAULT;
    socket_t *s = sock_from_fd(fd);
    if (!s || s->state != SOCK_CONNECTED) return -EBADF;
    int r = net_tcp_send(&s->tcp, buf, (int)len);
    return r < 0 ? -EIO : r;
}

/* ── SYS_RECVFROM (45) ───────────────────────────── */

long do_recvfrom(int fd, void *buf, long len, int flags,
                 void *src_addr, int *addrlen) {
    (void)flags; (void)src_addr; (void)addrlen;
    if (!user_ok((uint64_t)buf, (size_t)len)) return -EFAULT;
    socket_t *s = sock_from_fd(fd);
    if (!s || s->state != SOCK_CONNECTED) return -EBADF;
    int r = net_tcp_recv(&s->tcp, buf, (int)len, NET_TCP_TIMEOUT_MS);
    return r < 0 ? -EIO : r;
}

/* ── read/write/close via FD_SOCKET ──────────────── */

long socket_read(int fd, void *buf, long count) {
    socket_t *s = sock_from_fd(fd);
    if (!s || s->state != SOCK_CONNECTED) return -EBADF;
    int r = net_tcp_recv(&s->tcp, buf, (int)count, NET_TCP_TIMEOUT_MS);
    return r < 0 ? -EIO : r;
}

long socket_write(int fd, const void *buf, long count) {
    socket_t *s = sock_from_fd(fd);
    if (!s || s->state != SOCK_CONNECTED) return -EBADF;
    int r = net_tcp_send(&s->tcp, buf, (int)count);
    return r < 0 ? -EIO : r;
}

long socket_close(int fd) {
    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state == SOCK_CONNECTED)
        net_tcp_close(&s->tcp);
    s->state = SOCK_UNUSED;

    process_t *p = proc_current();
    if (p) fd_close(&p->fds, fd);
    return 0;
}

/* ── Stubs ───────────────────────────────────────── */

long do_bind(int fd, const void *addr, int addrlen) {
    (void)fd; (void)addr; (void)addrlen; return 0;
}
long do_listen(int fd, int backlog) {
    (void)fd; (void)backlog; return 0;
}
long do_accept(int fd, void *addr, int *addrlen) {
    (void)fd; (void)addr; (void)addrlen; return -ENOSYS;
}
long do_setsockopt(int fd, int level, int optname, const void *optval, int optlen) {
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen; return 0;
}
long do_getsockopt(int fd, int level, int optname, void *optval, int *optlen) {
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen; return 0;
}
long do_getsockname(int fd, void *addr, int *addrlen) {
    (void)fd; (void)addr; (void)addrlen; return 0;
}
long do_getpeername(int fd, void *addr, int *addrlen) {
    (void)fd; (void)addr; (void)addrlen; return 0;
}

/* ── SYS_POLL (7) ────────────────────────────────── */

struct k_pollfd { int fd; short events; short revents; };
#define POLLIN  0x0001
#define POLLOUT 0x0004

long do_poll(void *fds_ptr, int nfds, int timeout) {
    if (nfds <= 0 || nfds > 256) return -EINVAL;
    if (!user_ok((uint64_t)fds_ptr, (size_t)nfds * sizeof(struct k_pollfd)))
        return -EFAULT;

    struct k_pollfd *fds = (struct k_pollfd *)fds_ptr;
    uint64_t deadline = timer_ms() + (uint64_t)(timeout >= 0 ? timeout : 30000);
    int ready = 0;

    while (timer_ms() < deadline) {
        net_poll();
        ready = 0;
        for (int i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            socket_t *s = sock_from_fd(fds[i].fd);
            if (!s) {
                /* Non-socket FD: pretend writable */
                if (fds[i].events & POLLOUT) fds[i].revents |= POLLOUT;
                if (fds[i].revents) ready++;
                continue;
            }
            if ((fds[i].events & POLLIN) && s->state == SOCK_CONNECTED) {
                /* Check if TCP has buffered data or queue has packets */
                if (s->tcp.rxbuf_pos < s->tcp.rxbuf_len || q_tcp.count > 0)
                    fds[i].revents |= POLLIN;
            }
            if (fds[i].events & POLLOUT) {
                if (s->state == SOCK_CONNECTED) fds[i].revents |= POLLOUT;
            }
            if (fds[i].revents) ready++;
        }
        if (ready > 0) return ready;
        if (timeout == 0) return 0;
        __asm__ volatile("sti; hlt");
    }
    return 0;
}
