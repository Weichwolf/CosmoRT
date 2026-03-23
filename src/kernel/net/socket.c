/* CosmoRT Socket Layer — maps POSIX socket syscalls to net_tcp_* */

#include "socket.h"
#include "process.h"
#include "fd.h"
#include "serial.h"
#include "timer.h"
#include "syscall.h"
#include "spinlock.h"
#include "memops.h"
#include "config.h"
#include "percpu.h"

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
    (void)protocol;
    int base_type = type & 0xF; /* strip SOCK_CLOEXEC, SOCK_NONBLOCK */

    /* AF_UNIX (1): delegate to unix socket layer */
    if (domain == 1 /* AF_UNIX */) return usock_socket(type);

    /* Only AF_INET (2) + SOCK_STREAM (1) / SOCK_DGRAM (2) supported */
    if (domain != 2 /* AF_INET */) return -EAFNOSUPPORT;
    if (base_type != 1 && base_type != 2) return -EPROTONOSUPPORT;

    socket_t *s = sock_alloc();
    if (!s) return -EMFILE;
    s->refcount = 1;

    process_t *p = proc_current();
    if (!p) { s->state = SOCK_UNUSED; return -EFAULT; }

    int flags = 0x02; /* O_RDWR */
    if (type & 0x80000) flags |= 0x80000;      /* SOCK_CLOEXEC → O_CLOEXEC */
    if (type & 0x800) flags |= 0x800;          /* SOCK_NONBLOCK → O_NONBLOCK */

    int fd = fd_alloc(&p->fds, FD_SOCKET, s, flags);
    if (fd < 0) { s->state = SOCK_UNUSED; return -EMFILE; }
    return fd;
}

/* ── SYS_CONNECT (42) ────────────────────────────── */

long do_connect(int fd, const void *addr, int addrlen) {
    /* Check if AF_UNIX */
    process_t *cp = proc_current();
    if (cp) {
        fd_entry_t *fde = fd_get(&cp->fds, fd);
        if (fde && fde->type == FD_UNIX_SOCK)
            return usock_connect(fd, (const struct k_sockaddr_un *)addr, addrlen);
    }

    if (addrlen < (int)sizeof(struct k_sockaddr_in)) return -EINVAL;
    if (!user_ok((uint64_t)addr, (size_t)addrlen)) return -EFAULT;

    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state == SOCK_CONNECTED) return -EINVAL;

    /* Copy sockaddr to kernel to prevent TOCTOU */
    struct k_sockaddr_in k_addr;
    kmemcpy(&k_addr, addr, sizeof(k_addr));
    uint32_t ip_be = k_addr.sin_addr;
    uint8_t dst_ip[4] = {
        (uint8_t)(ip_be & 0xFF),
        (uint8_t)((ip_be >> 8) & 0xFF),
        (uint8_t)((ip_be >> 16) & 0xFF),
        (uint8_t)((ip_be >> 24) & 0xFF)
    };
    /* sin_port is big-endian, net_tcp_connect expects host uint16_t */
    uint16_t port = (uint16_t)((k_addr.sin_port >> 8) | (k_addr.sin_port << 8));

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
    /* Bounce user buffer to kernel to prevent TOCTOU with NIC DMA */
    uint8_t kbuf[1500];
    long todo = len > 1500 ? 1500 : len;
    kmemcpy(kbuf, buf, (size_t)todo);
    int r = net_tcp_send(&s->tcp, kbuf, (int)todo);
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
    /* Check if AF_UNIX */
    process_t *p = proc_current();
    if (p) {
        fd_entry_t *fde = fd_get(&p->fds, fd);
        if (fde && fde->type == FD_UNIX_SOCK)
            return usock_bind(fd, (const struct k_sockaddr_un *)addr, addrlen);
    }
    /* AF_INET bind: stub */
    (void)addr; (void)addrlen; return 0;
}
long do_listen(int fd, int backlog) {
    process_t *p = proc_current();
    if (p) {
        fd_entry_t *fde = fd_get(&p->fds, fd);
        if (fde && fde->type == FD_UNIX_SOCK)
            return usock_listen(fd, backlog);
    }
    (void)backlog; return 0;
}
long do_accept(int fd, void *addr, int *addrlen) {
    process_t *p = proc_current();
    if (p) {
        fd_entry_t *fde = fd_get(&p->fds, fd);
        if (fde && fde->type == FD_UNIX_SOCK)
            return usock_accept4(fd, addr, addrlen, 0);
    }
    (void)addr; (void)addrlen; return -ENOSYS;
}
long do_setsockopt(int fd, int level, int optname, const void *optval, int optlen) {
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen; return 0;
}
long do_getsockopt(int fd, int level, int optname, void *optval, int *optlen) {
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen; return 0;
}
long do_getsockname(int fd, void *addr, int *addrlen) {
    socket_t *s = sock_from_fd(fd);
    if (!s) return -ENOTSOCK;
    (void)addr; (void)addrlen;
    return 0;
}
long do_getpeername(int fd, void *addr, int *addrlen) {
    socket_t *s = sock_from_fd(fd);
    if (!s) return -ENOTSOCK;
    if (s->state != SOCK_CONNECTED) return -ENOTCONN;
    (void)addr; (void)addrlen;
    return 0;
}

/* ── SYS_POLL (7) ────────────────────────────────── */

struct k_pollfd { int fd; short events; short revents; };
#define POLLIN  0x0001
#define POLLOUT 0x0004

long do_poll(void *fds_ptr, int nfds, int timeout) {
    if (nfds <= 0 || nfds > 256) return -EINVAL;
    if (!user_ok((uint64_t)fds_ptr, (size_t)nfds * sizeof(struct k_pollfd)))
        return -EFAULT;

    /* Bounce user pollfd[] to kernel stack to prevent TOCTOU */
    struct k_pollfd kfds[256];
    kmemcpy(kfds, fds_ptr, (size_t)nfds * sizeof(struct k_pollfd));
<<<<<<< Updated upstream:src/kernel/net/socket.c

    uint64_t deadline = timer_ms() + (uint64_t)(timeout >= 0 ? timeout : 30000);

    net_poll();
    int ready = 0;
    for (int i = 0; i < nfds; i++) {
        kfds[i].revents = 0;
        socket_t *s = sock_from_fd(kfds[i].fd);
        if (!s) {
            if (kfds[i].events & POLLOUT) kfds[i].revents |= POLLOUT;
            if (kfds[i].revents) ready++;
            continue;
        }
        if ((kfds[i].events & POLLIN) && s->state == SOCK_CONNECTED) {
            if (s->tcp.rxbuf_pos < s->tcp.rxbuf_len || q_tcp.count > 0)
                kfds[i].revents |= POLLIN;
        }
        if (kfds[i].events & POLLOUT) {
            if (s->state == SOCK_CONNECTED) kfds[i].revents |= POLLOUT;
        }
        if (kfds[i].revents) ready++;
=======

    uint64_t deadline = timer_ms() + (uint64_t)(timeout >= 0 ? timeout : 30000);
    int ready = 0;

    while (timer_ms() < deadline) {
        net_poll();
        ready = 0;
        for (int i = 0; i < nfds; i++) {
            kfds[i].revents = 0;
            socket_t *s = sock_from_fd(kfds[i].fd);
            if (!s) {
                /* Non-socket FD: pretend writable */
                if (kfds[i].events & POLLOUT) kfds[i].revents |= POLLOUT;
                if (kfds[i].revents) ready++;
                continue;
            }
            if ((kfds[i].events & POLLIN) && s->state == SOCK_CONNECTED) {
                /* Check if TCP has buffered data or queue has packets */
                if (s->tcp.rxbuf_pos < s->tcp.rxbuf_len || q_tcp.count > 0)
                    kfds[i].revents |= POLLIN;
            }
            if (kfds[i].events & POLLOUT) {
                if (s->state == SOCK_CONNECTED) kfds[i].revents |= POLLOUT;
            }
            if (kfds[i].revents) ready++;
        }
        if (ready > 0) {
            /* Write back revents to user */
            kmemcpy(fds_ptr, kfds, (size_t)nfds * sizeof(struct k_pollfd));
            return ready;
        }
        if (timeout == 0) return 0;
        __asm__ volatile("sti; hlt");
>>>>>>> Stashed changes:src/kernel/socket.c
    }
    if (ready > 0) {
        kmemcpy(fds_ptr, kfds, (size_t)nfds * sizeof(struct k_pollfd));
        return ready;
    }
    if (timeout == 0) return 0;

    /* No events ready — block until woken by epoll_wake_all or timeout. */
    {
        extern uint64_t pml4[];
        extern void save_user_state_for_block(thread_t *t, long return_value);
        extern void thread_return_to_kernel(thread_t *t);
        extern void epoll_wake_all(void);

        thread_t *t = thread_current();
        if (!t) return -EFAULT;

        save_user_state_for_block(t, 0);
        t->rip -= 2;
        t->rax = SYS_POLL;
        t->wake_at = deadline;

        /* Register as sleeper (shares epoll sleeper list), then block. */
        extern void epoll_sleeper_add_ext(thread_t *t);
        epoll_sleeper_add_ext(t);
        t->state = THREAD_BLOCKED;
        __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(pml4)) : "memory");
        thread_return_to_kernel(t);
    }
    return 0; /* unreachable */
}
