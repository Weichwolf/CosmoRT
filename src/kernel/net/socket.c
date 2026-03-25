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
#include "epoll.h"
#include "arch.h"

/* sockaddr_in layout (user-space struct, 16 bytes) */
struct k_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;     /* big-endian */
    uint32_t sin_addr;     /* big-endian */
    uint8_t  sin_zero[8];
};

/* User-pointer validation + copy helpers */
#include "uaccess.h"

/* Socket pool */
static socket_t sockets[MAX_SOCKETS];
static spinlock_t sock_lock = SPINLOCK_INIT;

static socket_t *sock_alloc(void) {
    uint64_t flags;
    spin_lock_irq(&sock_lock, &flags);
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].state == SOCK_UNUSED) {
            /* Zero entire struct */
            for (int j = 0; j < (int)sizeof(socket_t); j++)
                ((uint8_t *)&sockets[i])[j] = 0;
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

/* Byte-swap helpers */
static inline uint16_t bswap16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
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
    s->is_dgram = (base_type == 2); /* SOCK_DGRAM */

    process_t *p = proc_current();
    if (!p) { s->state = SOCK_UNUSED; return -EFAULT; }

    int flags = 0x02; /* O_RDWR */
    if (type & 0x80000) flags |= 0x80000;      /* SOCK_CLOEXEC -> O_CLOEXEC */
    if (type & 0x800) flags |= 0x800;          /* SOCK_NONBLOCK -> O_NONBLOCK */

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

    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state == SOCK_CONNECTED && !s->is_dgram) return -EISCONN;
    if (s->state == SOCK_LISTENING) return -EISCONN;

    /* Copy sockaddr to kernel to prevent TOCTOU */
    struct k_sockaddr_in k_addr;
    { int r = copy_from_user(&k_addr, addr, sizeof(k_addr)); if (r) return r; }
    uint32_t ip_be = k_addr.sin_addr;
    uint8_t dst_ip[4] = {
        (uint8_t)(ip_be & 0xFF),
        (uint8_t)((ip_be >> 8) & 0xFF),
        (uint8_t)((ip_be >> 16) & 0xFF),
        (uint8_t)((ip_be >> 24) & 0xFF)
    };
    /* sin_port is big-endian, net_tcp_connect expects host uint16_t */
    uint16_t port = bswap16(k_addr.sin_port);

    /* UDP: just store destination, no handshake.
     * Don't change state — bind() may be called after connect() for UDP. */
    if (s->is_dgram) {
        s->remote_ip = k_addr.sin_addr;
        s->remote_port = k_addr.sin_port;
        return 0;
    }

    /* Zero the tcp struct */
    for (int i = 0; i < (int)sizeof(net_tcp_t); i++)
        ((uint8_t *)&s->tcp)[i] = 0;

    /* No gateway configured → network unreachable */
    if (net_gw_ip[0] == 0 && net_gw_ip[1] == 0 &&
        net_gw_ip[2] == 0 && net_gw_ip[3] == 0)
        return -ENETUNREACH;
    if (net_tcp_connect(&s->tcp, dst_ip, port) < 0) return -ETIMEDOUT;
    s->state = SOCK_CONNECTED;
    s->remote_ip = k_addr.sin_addr;
    s->remote_port = k_addr.sin_port;
    return 0;
}

/* ── SYS_SENDTO (44) ─────────────────────────────── */

long do_sendto(int fd, const void *buf, long len, int flags,
               const void *dest_addr, int addrlen) {
    (void)flags;
    if (!user_ok((uint64_t)buf, (size_t)len)) return -EFAULT;
    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->shut_wr) return -EPIPE;

    /* ── UDP (SOCK_DGRAM) ── */
    if (s->is_dgram) {
        /* Determine destination: from dest_addr or stored connect addr */
        uint32_t dst_ip_be;
        uint16_t dst_port_be;
        if (dest_addr && addrlen >= (int)sizeof(struct k_sockaddr_in)) {
            struct k_sockaddr_in ka;
            { int r = copy_from_user(&ka, dest_addr, sizeof(ka)); if (r) return r; }
            dst_ip_be = ka.sin_addr;
            dst_port_be = ka.sin_port;
        } else if (s->remote_ip) {
            dst_ip_be = s->remote_ip;
            dst_port_be = s->remote_port;
        } else {
            return -EDESTADDRREQ;
        }

        uint8_t dst_ip[4] = {
            (uint8_t)(dst_ip_be & 0xFF), (uint8_t)((dst_ip_be >> 8) & 0xFF),
            (uint8_t)((dst_ip_be >> 16) & 0xFF), (uint8_t)((dst_ip_be >> 24) & 0xFF)
        };
        uint16_t dst_port = bswap16(dst_port_be);

        /* Assign ephemeral local port if not bound */
        if (!s->udp_local_port) {
            extern int random_get(void *, unsigned long);
            uint16_t rnd;
            if (random_get(&rnd, sizeof(rnd)) < 0)
                rnd = (uint16_t)(timer_ms() & 0xFFFF);
            s->udp_local_port = (uint16_t)(49152 + (rnd & 0x3FFF));
            udp_bind(s->udp_local_port);
        }

        /* MTU guard: IP(20) + UDP(8) + payload ≤ 1500 */
        if (len > 1472) return -EMSGSIZE;

        /* Bounce user data to kernel buffer */
        uint8_t kbuf[1472];
        int slen = (int)len;
        { int r = copy_from_user(kbuf, buf, (size_t)slen); if (r) return r; }

        int r = net_udp_send(dst_ip, dst_port, s->udp_local_port, kbuf, slen);
        return r < 0 ? -EIO : (long)slen;
    }

    /* ── TCP (SOCK_STREAM) ── */
    if (s->state != SOCK_CONNECTED) return -EBADF;
    uint8_t kbuf[1460];
    const uint8_t *ubuf = (const uint8_t *)buf;
    long total = 0;
    while (total < len) {
        int chunk = (int)(len - total);
        if (chunk > 1460) chunk = 1460;
        copy_from_user(kbuf, ubuf + total, (size_t)chunk);
        int r = net_tcp_send(&s->tcp, kbuf, chunk);
        if (r < 0) return total > 0 ? total : -EIO;
        total += r;
    }
    return total;
}

/* ── SYS_RECVFROM (45) ───────────────────────────── */

long do_recvfrom(int fd, void *buf, long len, int flags,
                 void *src_addr, int *addrlen) {
    (void)flags;
    if (!user_ok((uint64_t)buf, (size_t)len)) return -EFAULT;
    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->shut_rd) return 0; /* EOF */

    /* ── UDP (SOCK_DGRAM) ── */
    if (s->is_dgram) {
        if (!s->udp_local_port) return -EAGAIN;
        /* Check non-blocking: O_NONBLOCK on fd or MSG_DONTWAIT */
        int nonblock = (flags & 0x40); /* MSG_DONTWAIT */
        { process_t *rp = proc_current();
          if (rp) { fd_entry_t *rf = fd_get(&rp->fds, fd);
                     if (rf && (rf->flags & O_NONBLOCK)) nonblock = 1; } }
        uint8_t kbuf[1400];
        uint8_t sip[4];
        uint16_t sport;
        int timeout = nonblock ? 0 : NET_TCP_TIMEOUT_MS;
        int r = net_udp_recv(s->udp_local_port, kbuf, (int)len > 1400 ? 1400 : (int)len,
                             sip, &sport, timeout);
        if (r < 0) return -EAGAIN;
        { int cr = copy_to_user(buf, kbuf, (size_t)r); if (cr) return cr; }

        /* Fill src_addr if provided */
        if (src_addr && addrlen) {
            struct k_sockaddr_in sa;
            for (int i = 0; i < (int)sizeof(sa); i++) ((uint8_t *)&sa)[i] = 0;
            sa.sin_family = 2; /* AF_INET */
            sa.sin_port = bswap16(sport);
            sa.sin_addr = (uint32_t)sip[0] | ((uint32_t)sip[1] << 8) |
                          ((uint32_t)sip[2] << 16) | ((uint32_t)sip[3] << 24);
            copy_to_user(src_addr, &sa, sizeof(sa));
            int slen = (int)sizeof(struct k_sockaddr_in);
            copy_to_user(addrlen, &slen, sizeof(int));
        }
        return r;
    }

    /* ── TCP (SOCK_STREAM) ── */
    if (s->state != SOCK_CONNECTED) return -EBADF;
    uint8_t kbuf[4096];
    int want = (int)len > 4096 ? 4096 : (int)len;
    int r = net_tcp_recv(&s->tcp, kbuf, want, NET_TCP_TIMEOUT_MS);
    if (r == -EAGAIN) return -EAGAIN;
    if (r < 0) return s->tcp.got_rst ? -ECONNRESET : -EIO;
    if (r > 0) { int cr = copy_to_user(buf, kbuf, (size_t)r); if (cr) return cr; }
    return r;
}

/* ── read/write/close via FD_SOCKET ──────────────── */

long socket_read(int fd, void *buf, long count) {
    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->shut_rd) return 0;
    /* DGRAM: delegate to recvfrom */
    if (s->is_dgram) return do_recvfrom(fd, buf, count, 0, 0, 0);
    if (s->state != SOCK_CONNECTED) return -EBADF;
    /* Check non-blocking */
    int nonblock = 0;
    { process_t *rp = proc_current();
      if (rp) { fd_entry_t *rf = fd_get(&rp->fds, fd);
                 if (rf && (rf->flags & O_NONBLOCK)) nonblock = 1; } }
    uint8_t kbuf[4096];
    int want = (int)count > 4096 ? 4096 : (int)count;
    int timeout = nonblock ? 0 : NET_TCP_TIMEOUT_MS;
    int r = net_tcp_recv(&s->tcp, kbuf, want, timeout);
    if (r == -EAGAIN) return -EAGAIN; /* timeout or non-blocking, no data */
    if (r < 0) return s->tcp.got_rst ? -ECONNRESET : -EIO;
    if (r > 0) { int cr = copy_to_user(buf, kbuf, (size_t)r); if (cr) return cr; }
    return r;
}

long socket_write(int fd, const void *buf, long count) {
    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->shut_wr) return -EPIPE;
    /* DGRAM: delegate to sendto (uses stored connect addr) */
    if (s->is_dgram) return do_sendto(fd, buf, count, 0, 0, 0);
    if (s->state != SOCK_CONNECTED) return -EBADF;
    long total = 0;
    while (total < count) {
        uint8_t kbuf[1460];
        int chunk = (int)(count - total);
        if (chunk > 1460) chunk = 1460;
        int cr = copy_from_user(kbuf, (const uint8_t *)buf + total, (size_t)chunk);
        if (cr) return total > 0 ? total : cr;
        int r = net_tcp_send(&s->tcp, kbuf, chunk);
        if (r < 0) return total > 0 ? total : -EIO;
        total += r;
    }
    return total;
}

long socket_close(int fd) {
    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->is_dgram && s->udp_local_port) {
        udp_sock_t *us = udp_find(s->udp_local_port);
        if (us) udp_unbind(us);
    }
    if (s->state == SOCK_CONNECTED && !s->is_dgram)
        net_tcp_close(&s->tcp);
    s->state = SOCK_UNUSED;

    process_t *p = proc_current();
    if (p) fd_close(&p->fds, fd);
    return 0;
}

/* ── SYS_BIND (49) ───────────────────────────────── */

long do_bind(int fd, const void *addr, int addrlen) {
    /* Check if AF_UNIX */
    process_t *p = proc_current();
    if (p) {
        fd_entry_t *fde = fd_get(&p->fds, fd);
        if (fde && fde->type == FD_UNIX_SOCK)
            return usock_bind(fd, (const struct k_sockaddr_un *)addr, addrlen);
    }

    if (addrlen < (int)sizeof(struct k_sockaddr_in)) return -EINVAL;

    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != SOCK_CREATED) return -EINVAL;

    struct k_sockaddr_in k_addr;
    { int r = copy_from_user(&k_addr, addr, sizeof(k_addr)); if (r) return r; }
    if (k_addr.sin_family != 2 /* AF_INET */) return -EAFNOSUPPORT;

    /* Check for port conflict (unless SO_REUSEADDR) */
    uint64_t lflags;
    spin_lock_irq(&sock_lock, &lflags);
    for (int i = 0; i < MAX_SOCKETS; i++) {
        socket_t *o = &sockets[i];
        if (o == s || o->state == SOCK_UNUSED) continue;
        if (o->local_port == k_addr.sin_port &&
            (o->local_ip == k_addr.sin_addr || k_addr.sin_addr == 0 || o->local_ip == 0)) {
            if (!(s->sockflags & SOCKF_REUSEADDR)) {
                spin_unlock_irq(&sock_lock, lflags);
                return -EADDRINUSE;
            }
        }
    }
    spin_unlock_irq(&sock_lock, lflags);

    s->local_ip = k_addr.sin_addr;
    s->local_port = k_addr.sin_port;
    /* For UDP: also set udp_local_port (host byte order) for sendto/recvfrom */
    if (s->is_dgram) {
        s->udp_local_port = bswap16(k_addr.sin_port);
        udp_bind(s->udp_local_port);
    }
    return 0;
}

/* ── SYS_LISTEN (50) ─────────────────────────────── */

long do_listen(int fd, int backlog) {
    process_t *p = proc_current();
    if (p) {
        fd_entry_t *fde = fd_get(&p->fds, fd);
        if (fde && fde->type == FD_UNIX_SOCK)
            return usock_listen(fd, backlog);
    }

    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != SOCK_CREATED) return -EINVAL;
    if (s->local_port == 0) return -EDESTADDRREQ;

    (void)backlog; /* accept queue is fixed at ACCEPT_QUEUE_MAX */
    s->state = SOCK_LISTENING;
    s->accept_head = 0;
    s->accept_count = 0;
    return 0;
}

/* ── SYS_ACCEPT (43) / SYS_ACCEPT4 (288) ─────────── */

long do_accept(int fd, void *addr, int *addrlen) {
    process_t *p = proc_current();
    if (p) {
        fd_entry_t *fde = fd_get(&p->fds, fd);
        if (fde && fde->type == FD_UNIX_SOCK)
            return usock_accept4(fd, addr, addrlen, 0);
    }

    socket_t *ls = sock_from_fd(fd);
    if (!ls) return -EBADF;
    if (ls->state != SOCK_LISTENING) return -EINVAL;

    /* Check accept queue first (pre-queued connections) */
    uint64_t flags;
    spin_lock_irq(&sock_lock, &flags);
    if (ls->accept_count > 0) {
        accept_conn_t *ac = &ls->accept_q[ls->accept_head];
        ls->accept_head = (ls->accept_head + 1) % ACCEPT_QUEUE_MAX;
        ls->accept_count--;

        socket_t *ns = 0;
        for (int i = 0; i < MAX_SOCKETS; i++) {
            if (sockets[i].state == SOCK_UNUSED) {
                for (int j = 0; j < (int)sizeof(socket_t); j++)
                    ((uint8_t *)&sockets[i])[j] = 0;
                ns = &sockets[i];
                break;
            }
        }
        if (!ns) { spin_unlock_irq(&sock_lock, flags); return -EMFILE; }

        kmemcpy(&ns->tcp, &ac->tcp, sizeof(net_tcp_t));
        ns->state = SOCK_CONNECTED;
        ns->refcount = 1;
        ns->local_ip = ls->local_ip;
        ns->local_port = ls->local_port;
        ns->remote_ip = ac->remote_ip;
        ns->remote_port = ac->remote_port;
        spin_unlock_irq(&sock_lock, flags);

        p = proc_current();
        if (!p) { ns->refcount = 0; ns->state = SOCK_UNUSED; return -EFAULT; }
        int newfd = fd_alloc(&p->fds, FD_SOCKET, ns, 0x02);
        if (newfd < 0) { ns->refcount = 0; ns->state = SOCK_UNUSED; return -EMFILE; }

        if (addr && addrlen) {
            struct k_sockaddr_in sa;
            for (int i = 0; i < (int)sizeof(sa); i++) ((uint8_t *)&sa)[i] = 0;
            sa.sin_family = 2;
            sa.sin_port = ac->remote_port;
            sa.sin_addr = ac->remote_ip;
            copy_to_user(addr, &sa, sizeof(sa));
            int len = (int)sizeof(struct k_sockaddr_in);
            copy_to_user(addrlen, &len, sizeof(int));
        }
        return newfd;
    }
    spin_unlock_irq(&sock_lock, flags);

    /* Non-blocking: return EAGAIN immediately if no pending connection */
    int nonblock = 0;
    { fd_entry_t *fde = fd_get(&p->fds, fd);
      if (fde && (fde->flags & O_NONBLOCK)) nonblock = 1; }
    if (nonblock) return -EAGAIN;

    /* No gateway → can only accept on loopback (not wired yet) → EAGAIN */
    if (net_gw_ip[0] == 0 && net_gw_ip[1] == 0 &&
        net_gw_ip[2] == 0 && net_gw_ip[3] == 0)
        return -EAGAIN;

    /* Block: do the server-side TCP handshake (SYN → SYN-ACK → ACK) */
    uint16_t host_port = bswap16(ls->local_port);
    net_tcp_t conn;
    if (net_tcp_accept(&conn, host_port, NET_TCP_TIMEOUT_MS) < 0)
        return -EAGAIN;

    /* Allocate new socket for the accepted connection */
    socket_t *ns = 0;
    spin_lock_irq(&sock_lock, &flags);
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].state == SOCK_UNUSED) {
            for (int j = 0; j < (int)sizeof(socket_t); j++)
                ((uint8_t *)&sockets[i])[j] = 0;
            ns = &sockets[i];
            break;
        }
    }
    if (!ns) { spin_unlock_irq(&sock_lock, flags); return -EMFILE; }

    kmemcpy(&ns->tcp, &conn, sizeof(net_tcp_t));
    ns->state = SOCK_CONNECTED;
    ns->refcount = 1;
    ns->local_ip = ls->local_ip;
    ns->local_port = ls->local_port;
    /* Store remote addr in network byte order */
    ns->remote_ip = (uint32_t)conn.dst_ip[0] |
                    ((uint32_t)conn.dst_ip[1] << 8) |
                    ((uint32_t)conn.dst_ip[2] << 16) |
                    ((uint32_t)conn.dst_ip[3] << 24);
    ns->remote_port = bswap16(conn.remote_port);
    spin_unlock_irq(&sock_lock, flags);

    /* Allocate FD */
    p = proc_current();
    if (!p) { ns->refcount = 0; ns->state = SOCK_UNUSED; return -EFAULT; }
    int newfd = fd_alloc(&p->fds, FD_SOCKET, ns, 0x02);
    if (newfd < 0) { ns->refcount = 0; ns->state = SOCK_UNUSED; return -EMFILE; }

    /* Fill in addr if requested */
    if (addr && addrlen) {
        struct k_sockaddr_in sa;
        for (int i = 0; i < (int)sizeof(sa); i++) ((uint8_t *)&sa)[i] = 0;
        sa.sin_family = 2;
        sa.sin_port = ns->remote_port;
        sa.sin_addr = ns->remote_ip;
        copy_to_user(addr, &sa, sizeof(sa));
        int len = (int)sizeof(struct k_sockaddr_in);
        copy_to_user(addrlen, &len, sizeof(int));
    }

    return newfd;
}

/* ── SYS_SETSOCKOPT (54) ─────────────────────────── */

long do_setsockopt(int fd, int level, int optname, const void *optval, int optlen) {
    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;

    if (optlen < (int)sizeof(int)) return -EINVAL;

    int val;
    { int r = copy_from_user(&val, optval, sizeof(int)); if (r) return r; }

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR:
            if (val) s->sockflags |= SOCKF_REUSEADDR;
            else     s->sockflags &= ~(uint32_t)SOCKF_REUSEADDR;
            return 0;
        case SO_KEEPALIVE:
            if (val) s->sockflags |= SOCKF_KEEPALIVE;
            else     s->sockflags &= ~(uint32_t)SOCKF_KEEPALIVE;
            return 0;
        default:
            return 0; /* silently accept unknown SOL_SOCKET opts */
        }
    }

    if (level == IPPROTO_TCP) {
        switch (optname) {
        case TCP_NODELAY:
            if (val) s->sockflags |= SOCKF_NODELAY;
            else     s->sockflags &= ~(uint32_t)SOCKF_NODELAY;
            return 0;
        default:
            return 0;
        }
    }

    return 0; /* unknown level: silently accept */
}

/* ── SYS_GETSOCKOPT (55) ─────────────────────────── */

long do_getsockopt(int fd, int level, int optname, void *optval, int *optlen) {
    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;

    int val = 0;

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR: val = (s->sockflags & SOCKF_REUSEADDR) ? 1 : 0; break;
        case SO_KEEPALIVE: val = (s->sockflags & SOCKF_KEEPALIVE) ? 1 : 0; break;
        default: break;
        }
    } else if (level == IPPROTO_TCP) {
        switch (optname) {
        case TCP_NODELAY: val = (s->sockflags & SOCKF_NODELAY) ? 1 : 0; break;
        default: break;
        }
    }

    { int r = copy_to_user(optval, &val, sizeof(int)); if (r) return r; }
    { int len = (int)sizeof(int); int r = copy_to_user(optlen, &len, sizeof(int)); if (r) return r; }
    return 0;
}

/* ── SYS_GETSOCKNAME (51) ────────────────────────── */

long do_getsockname(int fd, void *addr, int *addrlen) {
    socket_t *s = sock_from_fd(fd);
    if (!s) return -ENOTSOCK;

    struct k_sockaddr_in sa;
    for (int i = 0; i < (int)sizeof(sa); i++) ((uint8_t *)&sa)[i] = 0;
    sa.sin_family = 2; /* AF_INET */
    sa.sin_port = s->local_port;
    sa.sin_addr = s->local_ip;

    { int r = copy_to_user(addr, &sa, sizeof(sa)); if (r) return r; }
    { int len = (int)sizeof(struct k_sockaddr_in); int r = copy_to_user(addrlen, &len, sizeof(int)); if (r) return r; }
    return 0;
}

/* ── SYS_GETPEERNAME (52) ────────────────────────── */

long do_getpeername(int fd, void *addr, int *addrlen) {
    socket_t *s = sock_from_fd(fd);
    if (!s) return -ENOTSOCK;
    if (s->state != SOCK_CONNECTED) return -ENOTCONN;

    struct k_sockaddr_in sa;
    for (int i = 0; i < (int)sizeof(sa); i++) ((uint8_t *)&sa)[i] = 0;
    sa.sin_family = 2; /* AF_INET */
    sa.sin_port = s->remote_port;
    sa.sin_addr = s->remote_ip;

    { int r = copy_to_user(addr, &sa, sizeof(sa)); if (r) return r; }
    { int len = (int)sizeof(struct k_sockaddr_in); int r = copy_to_user(addrlen, &len, sizeof(int)); if (r) return r; }
    return 0;
}

/* ── SYS_SHUTDOWN (48) ───────────────────────────── */

long do_shutdown(int fd, int how) {
    /* Check AF_UNIX first */
    process_t *p = proc_current();
    if (p) {
        fd_entry_t *fde = fd_get(&p->fds, fd);
        if (fde && fde->type == FD_UNIX_SOCK)
            return 0; /* AF_UNIX shutdown: no-op for now */
    }

    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != SOCK_CONNECTED && s->state != SOCK_LISTENING)
        return -ENOTCONN;

    switch (how) {
    case 0 /* SHUT_RD */:
        s->shut_rd = 1;
        break;
    case 1 /* SHUT_WR */:
        s->shut_wr = 1;
        if (s->state == SOCK_CONNECTED && !s->is_dgram)
            net_tcp_close(&s->tcp); /* sends FIN */
        break;
    case 2 /* SHUT_RDWR */:
        s->shut_rd = 1;
        s->shut_wr = 1;
        if (s->state == SOCK_CONNECTED && !s->is_dgram)
            net_tcp_close(&s->tcp);
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

/* ── SYS_POLL (7) ────────────────────────────────── */

struct k_pollfd { int fd; short events; short revents; };
#define POLLIN  0x0001
#define POLLOUT 0x0004

long do_poll(void *fds_ptr, int nfds, int timeout) {
    if (nfds <= 0 || nfds > 256) return -EINVAL;
    /* Bounce user pollfd[] to kernel stack to prevent TOCTOU */
    struct k_pollfd kfds[256];
    { int r = copy_from_user(kfds, fds_ptr, (size_t)nfds * sizeof(struct k_pollfd)); if (r) return r; }

    int infinite = (timeout < 0);
    uint64_t deadline = infinite ? 0 : timer_ms() + (uint64_t)timeout;
    int ready = 0;

    while (infinite || timer_ms() < deadline) {
        ready = 0;
        for (int i = 0; i < nfds; i++) {
            kfds[i].revents = 0;
            /* Map POLLIN/POLLOUT to EPOLLIN/EPOLLOUT for fd_poll_readiness */
            uint32_t interest = 0;
            if (kfds[i].events & POLLIN)  interest |= 0x001; /* EPOLLIN */
            if (kfds[i].events & POLLOUT) interest |= 0x004; /* EPOLLOUT */
            uint32_t r_ev = fd_poll_readiness(kfds[i].fd, interest);
            if (r_ev & 0x001) kfds[i].revents |= POLLIN;
            if (r_ev & 0x004) kfds[i].revents |= POLLOUT;
            if (r_ev & 0x010) kfds[i].revents |= 0x0010; /* POLLHUP */
            if (r_ev & 0x008) kfds[i].revents |= 0x0008; /* POLLERR */
            if (kfds[i].revents) ready++;
        }
        if (ready > 0) {
            copy_to_user(fds_ptr, kfds, (size_t)nfds * sizeof(struct k_pollfd));
            return ready;
        }
        if (timeout == 0) return 0;
        arch_halt();
    }
    if (ready > 0) {
        copy_to_user(fds_ptr, kfds, (size_t)nfds * sizeof(struct k_pollfd));
        return ready;
    }
    if (timeout == 0) return 0;

    /* No events ready -- block until woken by epoll_wake_all or timeout. */
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
        t->wake_at = infinite ? 0 : deadline; /* 0 = no timeout */

        /* Register as sleeper (shares epoll sleeper list), then block. */
        extern void epoll_sleeper_add_ext(thread_t *t);
        epoll_sleeper_add_ext(t);
        t->state = THREAD_BLOCKED;
        arch_set_cr3(virt_to_phys(pml4));
        thread_return_to_kernel(t);
    }
    return 0; /* unreachable */
}
