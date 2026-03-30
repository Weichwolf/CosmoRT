/* CosmoRT Socket Layer — maps POSIX socket syscalls to net_tcp_* */

#include "net/socket.h"
#include "proc/process.h"
#include "event/fd.h"
#include "hw/serial.h"
#include "core/timer.h"
#include "sys/syscall.h"
#include "core/mutex.h"
#include "memops.h"
#include "config.h"
#include "core/percpu.h"
#include "event/epoll.h"
#include "core/event_queue.h"

extern long send_sigpipe(void);
#include "arch/arch.h"
#include "core/rt.h"
#include "core/timer_wheel.h"

struct k_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
};

#include "uaccess.h"

static socket_t sock_pool[SOCK_SLAB_CAP];
slab_t sock_slab;
socket_t *sock_active_head;
static mutex_t sock_lock = MUTEX_INIT;
static int sock_slab_inited;

static void sock_slab_ensure_init(void) {
    if (__builtin_expect(sock_slab_inited, 1)) return;
    slab_init(&sock_slab, sock_pool, (int)sizeof(socket_t), SOCK_SLAB_CAP);
    sock_active_head = 0;
    sock_slab_inited = 1;
}

static void sock_list_add(socket_t *s) {
    s->prev_active = 0;
    s->next_active = sock_active_head;
    if (sock_active_head) sock_active_head->prev_active = s;
    sock_active_head = s;
}

static void sock_list_del(socket_t *s) {
    if (s->prev_active) s->prev_active->next_active = s->next_active;
    else                 sock_active_head = s->next_active;
    if (s->next_active) s->next_active->prev_active = s->prev_active;
    s->next_active = 0;
    s->prev_active = 0;
}

socket_t *sock_alloc(void) {
    mutex_lock(&sock_lock);
    sock_slab_ensure_init();
    socket_t *s = (socket_t *)slab_alloc(&sock_slab);
    if (s) {
        s->state = SOCK_CREATED;
        sock_list_add(s);
    }
    mutex_unlock(&sock_lock);
    return s;
}

void sock_free(socket_t *s) {
    if (!s) return;
    mutex_lock(&sock_lock);
    sock_list_del(s);
    slab_free(&sock_slab, s);
    mutex_unlock(&sock_lock);
}

static socket_t *sock_from_fd(int fd) {
    process_t *p = proc_current();
    if (!p) return 0;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_SOCKET) return 0;
    return (socket_t *)fde->obj;
}

static void sock_finalize_connect(socket_t *s) {
    if ((s->sockflags & SOCKF_CONNECTING) && s->tcp.state == TCP_ESTABLISHED) {
        s->state = SOCK_CONNECTED;
        s->sockflags &= ~(uint32_t)SOCKF_CONNECTING;
        s->tcp.wait_thread = 0;
    }
}

static inline uint16_t bswap16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

long do_socket(int domain, int type, int protocol) {
    (void)protocol;
    int base_type = type & 0xF;

    if (domain == 1) return usock_socket(type);

    if (domain != 2) return -EAFNOSUPPORT;
    if (base_type != 1 && base_type != 2) return -EPROTONOSUPPORT;

    socket_t *s = sock_alloc();
    if (!s) return -EMFILE;
    s->refcount = 1;
    s->is_dgram = (base_type == 2);

    process_t *p = proc_current();
    if (!p) { sock_free(s); return -EFAULT; }

    int flags = 0x02;
    if (type & 0x80000) flags |= 0x80000;
    if (type & 0x800) flags |= 0x800;

    int fd = fd_alloc(&p->fds, FD_SOCKET, s, flags);
    if (fd < 0) { sock_free(s); return -EMFILE; }
    return fd;
}

long do_connect(int fd, const void *addr, int addrlen) {
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

    if (s->sockflags & SOCKF_CONNECTING) {
        if (s->tcp.state == TCP_ESTABLISHED) {
            s->state = SOCK_CONNECTED;
            s->sockflags &= ~(uint32_t)SOCKF_CONNECTING;
            s->tcp.wait_thread = 0;
            return 0;
        }
        if (s->tcp.got_rst) {
            s->sockflags &= ~(uint32_t)SOCKF_CONNECTING;
            return -ECONNREFUSED;
        }
        return -EALREADY;
    }

    struct k_sockaddr_in k_addr;
    { int r = copy_from_user(&k_addr, addr, sizeof(k_addr)); if (r) return r; }
    uint32_t ip_be = k_addr.sin_addr;
    uint8_t dst_ip[4] = {
        (uint8_t)(ip_be & 0xFF),
        (uint8_t)((ip_be >> 8) & 0xFF),
        (uint8_t)((ip_be >> 16) & 0xFF),
        (uint8_t)((ip_be >> 24) & 0xFF)
    };
    uint16_t port = bswap16(k_addr.sin_port);

    if (s->is_dgram) {
        s->remote_ip = k_addr.sin_addr;
        s->remote_port = k_addr.sin_port;
        return 0;
    }

    if (s->tcp.state == TCP_CLOSED && !s->tcp.got_rst)
        for (int i = 0; i < (int)sizeof(net_tcp_t); i++)
            ((uint8_t *)&s->tcp)[i] = 0;

    if (net_gw_ip[0] == 0 && net_gw_ip[1] == 0 &&
        net_gw_ip[2] == 0 && net_gw_ip[3] == 0)
        return -ENETUNREACH;

    uint64_t connect_deadline = timer_ms() + NET_TCP_TIMEOUT_MS;
    for (;;) {
        int r = net_tcp_connect(&s->tcp, dst_ip, port);
        if (r == 0) {
            s->state = SOCK_CONNECTED;
            s->remote_ip = k_addr.sin_addr;
            s->remote_port = k_addr.sin_port;
            s->tcp.wait_thread = 0;
            s->sockflags &= ~(uint32_t)SOCKF_CONNECTING;
            return 0;
        }
        if (r == -11) {
            int nonblock = 0;
            { process_t *rp = proc_current();
              if (rp) { fd_entry_t *rf = fd_get(&rp->fds, fd);
                         if (rf && (rf->flags & O_NONBLOCK)) nonblock = 1; } }
            if (nonblock) {
                s->sockflags |= SOCKF_CONNECTING;
                s->remote_ip = k_addr.sin_addr;
                s->remote_port = k_addr.sin_port;
                return -EINPROGRESS;
            }
            if (timer_ms() >= connect_deadline) return -ETIMEDOUT;
            thread_t *t = thread_current();
            __atomic_store_n(&s->tcp.wait_thread, t, __ATOMIC_RELEASE);
            int remain = (int)(connect_deadline - timer_ms());
            if (remain <= 0) return -ETIMEDOUT;
            event_t ev;
            int wr = event_wait(&t->eq, &ev, remain);
            if (wr == -4) return -EINTR;
            continue;
        }
        return -ETIMEDOUT;
    }
}

long do_sendto(int fd, const void *buf, long len, int flags,
               const void *dest_addr, int addrlen) {
    (void)flags;
    if (!user_ok((uint64_t)buf, (size_t)len)) return -EFAULT;
    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->shut_wr) return send_sigpipe();

    if (s->is_dgram) {
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

        if (!s->udp_local_port) {
            extern int random_get(void *, unsigned long);
            uint16_t rnd;
            if (random_get(&rnd, sizeof(rnd)) < 0)
                rnd = (uint16_t)(timer_ms() & 0xFFFF);
            s->udp_local_port = (uint16_t)(49152 + (rnd & 0x3FFF));
            udp_bind(s->udp_local_port);
        }

        if (len > 1472) return -EMSGSIZE;

        uint8_t kbuf[1472];
        int slen = (int)len;
        { int r = copy_from_user(kbuf, buf, (size_t)slen); if (r) return r; }

        int r = net_udp_send(dst_ip, dst_port, s->udp_local_port, kbuf, slen);
        return r < 0 ? -EIO : (long)slen;
    }

    sock_finalize_connect(s);
    if (s->state != SOCK_CONNECTED) return -ENOTCONN;
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

long do_recvfrom(int fd, void *buf, long len, int flags,
                 void *src_addr, int *addrlen) {
    (void)flags;
    if (!user_ok((uint64_t)buf, (size_t)len)) return -EFAULT;
    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->shut_rd) return 0;

    if (s->is_dgram) {
        if (!s->udp_local_port) return -EAGAIN;
        int nonblock_udp = (flags & 0x40);
        { process_t *rp = proc_current();
          if (rp) { fd_entry_t *rf = fd_get(&rp->fds, fd);
                     if (rf && (rf->flags & O_NONBLOCK)) nonblock_udp = 1; } }
        uint64_t udp_deadline = timer_ms() + NET_TCP_TIMEOUT_MS;
        uint8_t kbuf[1400];
        uint8_t sip[4];
        uint16_t sport;
        for (;;) {
            int r = net_udp_recv(s->udp_local_port, kbuf, (int)len > 1400 ? 1400 : (int)len,
                                 sip, &sport, 0);
            if (r >= 0) {
                { int cr = copy_to_user(buf, kbuf, (size_t)r); if (cr) return cr; }
                if (src_addr && addrlen) {
                    struct k_sockaddr_in sa;
                    for (int i = 0; i < (int)sizeof(sa); i++) ((uint8_t *)&sa)[i] = 0;
                    sa.sin_family = 2;
                    sa.sin_port = bswap16(sport);
                    sa.sin_addr = (uint32_t)sip[0] | ((uint32_t)sip[1] << 8) |
                                  ((uint32_t)sip[2] << 16) | ((uint32_t)sip[3] << 24);
                    copy_to_user(src_addr, &sa, sizeof(sa));
                    int slen = (int)sizeof(struct k_sockaddr_in);
                    copy_to_user(addrlen, &slen, sizeof(int));
                }
                return r;
            }
            if (nonblock_udp) return -EAGAIN;
            if (timer_ms() >= udp_deadline) return -EAGAIN;
            udp_sock_t *us = udp_find(s->udp_local_port);
            if (!us) return -EAGAIN;
            thread_t *t = thread_current();
            __atomic_store_n(&us->wait_thread, t, __ATOMIC_RELEASE);
            int remain = (int)(udp_deadline - timer_ms());
            if (remain <= 0) return -EAGAIN;
            event_t ev;
            { int wr = event_wait(&t->eq, &ev, remain);
            if (wr == -4) return -EINTR; }
        }
    }

    sock_finalize_connect(s);
    if (s->state != SOCK_CONNECTED) return -ENOTCONN;
    {
        int nonblock = (flags & 0x40);
        { process_t *rp = proc_current();
          if (rp) { fd_entry_t *rf = fd_get(&rp->fds, fd);
                     if (rf && (rf->flags & O_NONBLOCK)) nonblock = 1; } }
        uint64_t now = timer_ms();
        if (s->recv_deadline) {
            if (now >= s->recv_deadline) { s->recv_deadline = 0; return -EAGAIN; }
        } else {
            uint64_t timeo = s->tcp.rcv_timeo_ms ? s->tcp.rcv_timeo_ms : NET_TCP_TIMEOUT_MS;
            s->recv_deadline = now + timeo;
        }
        for (;;) {
            uint8_t kbuf[4096];
            int want = (int)len > 4096 ? 4096 : (int)len;
            int r = net_tcp_recv(&s->tcp, kbuf, want, 0);
            if (r != -11) {
                s->tcp.wait_thread = 0;
                s->recv_deadline = 0;
                if (r < 0) return s->tcp.got_rst ? -ECONNRESET : -EIO;
                if (r > 0) { int cr = copy_to_user(buf, kbuf, (size_t)r); if (cr) return cr; }
                return r;
            }
            if (nonblock) { s->recv_deadline = 0; return -EAGAIN; }
            if (timer_ms() >= s->recv_deadline) { s->recv_deadline = 0; return -EAGAIN; }
            thread_t *t = thread_current();
            __atomic_store_n(&s->tcp.wait_thread, t, __ATOMIC_RELEASE);
            int remain = (int)(s->recv_deadline - timer_ms());
            if (remain <= 0) { s->recv_deadline = 0; return -EAGAIN; }
            event_t ev;
            { int wr = event_wait(&t->eq, &ev, remain);
            if (wr == -4) return -EINTR; }
        }
    }
}

long socket_read(int fd, void *buf, long count) {
    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->shut_rd) return 0;
    if (s->is_dgram) return do_recvfrom(fd, buf, count, 0, 0, 0);
    sock_finalize_connect(s);
    if (s->state != SOCK_CONNECTED) return -ENOTCONN;
    {
        int nonblock = 0;
        { process_t *rp = proc_current();
          if (rp) { fd_entry_t *rf = fd_get(&rp->fds, fd);
                     if (rf && (rf->flags & O_NONBLOCK)) nonblock = 1; } }
        uint64_t now = timer_ms();
        if (s->recv_deadline) {
            if (now >= s->recv_deadline) { s->recv_deadline = 0; return -EAGAIN; }
        } else {
            uint64_t timeo = s->tcp.rcv_timeo_ms ? s->tcp.rcv_timeo_ms : NET_TCP_TIMEOUT_MS;
            s->recv_deadline = now + timeo;
        }
        for (;;) {
            uint8_t kbuf[4096];
            int want = (int)count > 4096 ? 4096 : (int)count;
            int r = net_tcp_recv(&s->tcp, kbuf, want, 0);
            if (r != -11) {
                s->tcp.wait_thread = 0;
                s->recv_deadline = 0;
                if (r < 0) return s->tcp.got_rst ? -ECONNRESET : -EIO;
                if (r > 0) { int cr = copy_to_user(buf, kbuf, (size_t)r); if (cr) return cr; }
                return r;
            }
            if (nonblock) { s->recv_deadline = 0; return -EAGAIN; }
            if (timer_ms() >= s->recv_deadline) { s->recv_deadline = 0; return -EAGAIN; }
            thread_t *t = thread_current();
            __atomic_store_n(&s->tcp.wait_thread, t, __ATOMIC_RELEASE);
            int remain = (int)(s->recv_deadline - timer_ms());
            if (remain <= 0) { s->recv_deadline = 0; return -EAGAIN; }
            event_t ev;
            { int wr = event_wait(&t->eq, &ev, remain);
            if (wr == -4) return -EINTR; }
        }
    }
}

long socket_write(int fd, const void *buf, long count) {
    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->shut_wr) return send_sigpipe();
    if (s->is_dgram) return do_sendto(fd, buf, count, 0, 0, 0);
    sock_finalize_connect(s);
    if (s->state != SOCK_CONNECTED) return -ENOTCONN;
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

    process_t *p = proc_current();
    if (p) fd_close(&p->fds, fd);

    int old = __sync_fetch_and_sub(&s->refcount, 1);
    if (old > 1) return 0;

    if (s->is_dgram && s->udp_local_port) {
        udp_sock_t *us = udp_find(s->udp_local_port);
        if (us) udp_unbind(us);
    }
    if (s->state == SOCK_CONNECTED && !s->is_dgram)
        net_tcp_close(&s->tcp);
    sock_free(s);
    return 0;
}

long do_bind(int fd, const void *addr, int addrlen) {
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
    if (k_addr.sin_family != 2) return -EAFNOSUPPORT;

    if (k_addr.sin_port == 0) {
        extern int random_get(void *, unsigned long);
        uint16_t rnd;
        if (random_get(&rnd, sizeof(rnd)) < 0)
            rnd = (uint16_t)(timer_ms() & 0xFFFF);
        mutex_lock(&sock_lock);
        for (int attempt = 0; attempt < 128; attempt++) {
            uint16_t port_host = (uint16_t)(49152 + ((rnd + attempt) & 0x3FFF));
            uint16_t port_be = bswap16(port_host);
            int conflict = 0;
            for (socket_t *o = sock_active_head; o; o = o->next_active) {
                if (o == s) continue;
                if (o->local_port == port_be) { conflict = 1; break; }
            }
            if (!conflict) {
                s->local_ip = k_addr.sin_addr;
                s->local_port = port_be;
                if (s->is_dgram) {
                    s->udp_local_port = port_host;
                    mutex_unlock(&sock_lock);
                    udp_bind(port_host);
                    return 0;
                }
                mutex_unlock(&sock_lock);
                return 0;
            }
        }
        mutex_unlock(&sock_lock);
        return -EADDRINUSE;
    }

    mutex_lock(&sock_lock);
    for (socket_t *o = sock_active_head; o; o = o->next_active) {
        if (o == s) continue;
        if (o->local_port == k_addr.sin_port &&
            (o->local_ip == k_addr.sin_addr || k_addr.sin_addr == 0 || o->local_ip == 0)) {
            if (!(s->sockflags & SOCKF_REUSEADDR)) {
                mutex_unlock(&sock_lock);
                return -EADDRINUSE;
            }
        }
    }
    mutex_unlock(&sock_lock);

    s->local_ip = k_addr.sin_addr;
    s->local_port = k_addr.sin_port;
    if (s->is_dgram) {
        s->udp_local_port = bswap16(k_addr.sin_port);
        udp_bind(s->udp_local_port);
    }
    return 0;
}

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

    (void)backlog;
    s->state = SOCK_LISTENING;
    s->accept_head = 0;
    s->accept_count = 0;
    return 0;
}

long do_accept(int fd, void *addr, int *addrlen) {
    process_t *p = proc_current();
    if (p) {
        fd_entry_t *fde = fd_get(&p->fds, fd);
        if (fde && fde->type == FD_UNIX_SOCK)
            return usock_accept4(fd, addr, addrlen, 0);
    }

    socket_t *ls = sock_from_fd(fd);
    if (!ls) {
        process_t *ap = proc_current();
        if (ap) {
            fd_entry_t *fde = fd_get(&ap->fds, fd);
            if (fde && !(fde->flags & O_PATH)) return -ENOTSOCK;
        }
        return -EBADF;
    }
    if (ls->is_dgram) return -EOPNOTSUPP;
    if (ls->state != SOCK_LISTENING) return -EINVAL;

    mutex_lock(&sock_lock);
    if (ls->accept_count > 0) {
        accept_conn_t ac;
        kmemcpy(&ac, &ls->accept_q[ls->accept_head], sizeof(accept_conn_t));
        ls->accept_head = (ls->accept_head + 1) % ACCEPT_QUEUE_MAX;
        ls->accept_count--;
        uint32_t ls_local_ip = ls->local_ip;
        uint16_t ls_local_port = ls->local_port;
        mutex_unlock(&sock_lock);

        socket_t *ns = sock_alloc();
        if (!ns) return -EMFILE;

        kmemcpy(&ns->tcp, &ac.tcp, sizeof(net_tcp_t));
        ns->state = SOCK_CONNECTED;
        ns->refcount = 1;
        ns->local_ip = ls_local_ip;
        ns->local_port = ls_local_port;
        ns->remote_ip = ac.remote_ip;
        ns->remote_port = ac.remote_port;

        p = proc_current();
        if (!p) { sock_free(ns); return -EFAULT; }
        int newfd = fd_alloc(&p->fds, FD_SOCKET, ns, 0x02);
        if (newfd < 0) { sock_free(ns); return -EMFILE; }

        if (addr && addrlen) {
            struct k_sockaddr_in sa;
            for (int i = 0; i < (int)sizeof(sa); i++) ((uint8_t *)&sa)[i] = 0;
            sa.sin_family = 2;
            sa.sin_port = ac.remote_port;
            sa.sin_addr = ac.remote_ip;
            copy_to_user(addr, &sa, sizeof(sa));
            int len = (int)sizeof(struct k_sockaddr_in);
            copy_to_user(addrlen, &len, sizeof(int));
        }
        return newfd;
    }
    mutex_unlock(&sock_lock);

    int nonblock = 0;
    { fd_entry_t *fde = fd_get(&p->fds, fd);
      if (fde && (fde->flags & O_NONBLOCK)) nonblock = 1; }
    if (nonblock) return -EAGAIN;

    uint16_t host_port = bswap16(ls->local_port);
    uint64_t accept_deadline = timer_ms() + NET_TCP_TIMEOUT_MS;
    for (;;) {
        int r = net_tcp_accept(&ls->tcp, host_port, 0);
        if (r != -11) {
            if (r < 0) { ls->tcp.state = TCP_CLOSED; return -EAGAIN; }
            break;
        }
        if (timer_ms() >= accept_deadline) return -EAGAIN;
        thread_t *t = thread_current();
        __atomic_store_n(&q_tcp_wait_thread, t, __ATOMIC_RELEASE);
        int remain = (int)(accept_deadline - timer_ms());
        if (remain <= 0) return -EAGAIN;
        event_t ev;
        int wr = event_wait(&t->eq, &ev, remain);
        if (wr == -4) return -EINTR;
    }

    q_tcp_wait_thread = 0;
    socket_t *ns = sock_alloc();
    if (!ns) return -EMFILE;

    kmemcpy(&ns->tcp, &ls->tcp, sizeof(net_tcp_t));
    ns->tcp.wait_thread = 0;
    ns->state = SOCK_CONNECTED;
    ns->refcount = 1;
    ns->local_ip = ls->local_ip;
    ns->local_port = ls->local_port;
    ns->remote_ip = (uint32_t)ls->tcp.dst_ip[0] |
                    ((uint32_t)ls->tcp.dst_ip[1] << 8) |
                    ((uint32_t)ls->tcp.dst_ip[2] << 16) |
                    ((uint32_t)ls->tcp.dst_ip[3] << 24);
    ns->remote_port = bswap16(ls->tcp.remote_port);

    kmemset(&ls->tcp, 0, sizeof(net_tcp_t));

    p = proc_current();
    if (!p) { sock_free(ns); return -EFAULT; }
    int newfd = fd_alloc(&p->fds, FD_SOCKET, ns, 0x02);
    if (newfd < 0) { sock_free(ns); return -EMFILE; }

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
            if (val) {
                s->sockflags |= SOCKF_KEEPALIVE;
                s->tcp.keepalive = 1;
                s->tcp.keepalive_probes = 0;
                if (s->tcp.state == TCP_ESTABLISHED) {
                    extern int rt_timer_request(uint8_t action, void *ctx, uint32_t timeout_ms);
                    rt_timer_request(RT_TIMER_TCP_KEEPALIVE, &s->tcp,
                                     NET_TCP_KEEPALIVE_INTERVAL_MS);
                    extern uint64_t timer_ms(void);
                    s->tcp.keepalive_next = timer_ms() + NET_TCP_KEEPALIVE_INTERVAL_MS;
                }
            } else {
                s->sockflags &= ~(uint32_t)SOCKF_KEEPALIVE;
                s->tcp.keepalive = 0;
            }
            return 0;
        case SO_RCVTIMEO: {
            if (optlen >= 16) {
                long tv[2];
                { int r = copy_from_user(tv, optval, 16); if (r) return r; }
                s->tcp.rcv_timeo_ms = (uint64_t)tv[0] * 1000 + (uint64_t)tv[1] / 1000;
            } else {
                s->tcp.rcv_timeo_ms = (uint64_t)(val > 0 ? val : 0);
            }
            return 0;
        }
        case SO_SNDTIMEO: {
            if (optlen >= 16) {
                long tv[2];
                { int r = copy_from_user(tv, optval, 16); if (r) return r; }
                s->tcp.snd_timeo_ms = (uint64_t)tv[0] * 1000 + (uint64_t)tv[1] / 1000;
            } else {
                s->tcp.snd_timeo_ms = (uint64_t)(val > 0 ? val : 0);
            }
            return 0;
        }
        case SO_SNDBUF:
        case SO_RCVBUF:
            return 0;
        case SO_BROADCAST:
        case SO_OOBINLINE:
        case SO_REUSEPORT:
            return 0;
        case SO_LINGER:
            return 0;
        default:
            return 0;
        }
    }

    if (level == IPPROTO_TCP) {
        switch (optname) {
        case TCP_NODELAY:
            if (val) s->sockflags |= SOCKF_NODELAY;
            else     s->sockflags &= ~(uint32_t)SOCKF_NODELAY;
            return 0;
        case TCP_CORK:
        case TCP_QUICKACK:
            return 0;
        case TCP_KEEPIDLE:
        case TCP_KEEPINTVL:
        case TCP_KEEPCNT:
            return 0;
        case TCP_FASTOPEN:
        case TCP_FASTOPEN_CONNECT:
            s->tcp.tfo_enabled = val ? 1 : 0;
            return 0;
        default:
            return 0;
        }
    }

    return 0;
}

long do_getsockopt(int fd, int level, int optname, void *optval, int *optlen) {
    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;

    int val = 0;

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR: val = (s->sockflags & SOCKF_REUSEADDR) ? 1 : 0; break;
        case SO_KEEPALIVE: val = (s->sockflags & SOCKF_KEEPALIVE) ? 1 : 0; break;
        case SO_SNDBUF: val = 65536; break;
        case SO_RCVBUF: val = 65536; break;
        case SO_BROADCAST: val = 0; break;
        case SO_OOBINLINE: val = 0; break;
        case SO_REUSEPORT: val = 0; break;
        case SO_LINGER: {
            int linger[2] = { 0, 0 };
            { int r = copy_to_user(optval, linger, 8); if (r) return r; }
            { int len = 8; int r = copy_to_user(optlen, &len, sizeof(int)); if (r) return r; }
            return 0;
        }
        case SO_ERROR:
            if (s->sockflags & SOCKF_CONNECTING) {
                if (s->tcp.got_rst) {
                    val = ECONNREFUSED;
                    s->sockflags &= ~(uint32_t)SOCKF_CONNECTING;
                } else if (s->tcp.state == TCP_ESTABLISHED) {
                    val = 0;
                    s->state = SOCK_CONNECTED;
                    s->sockflags &= ~(uint32_t)SOCKF_CONNECTING;
                    s->tcp.wait_thread = 0;
                } else {
                    val = EINPROGRESS;
                }
            }
            break;
        case SO_RCVTIMEO: {
            long tv[2] = { (long)(s->tcp.rcv_timeo_ms / 1000),
                           (long)((s->tcp.rcv_timeo_ms % 1000) * 1000) };
            { int r = copy_to_user(optval, tv, 16); if (r) return r; }
            { int len = 16; int r = copy_to_user(optlen, &len, sizeof(int)); if (r) return r; }
            return 0;
        }
        case SO_SNDTIMEO: {
            long tv[2] = { (long)(s->tcp.snd_timeo_ms / 1000),
                           (long)((s->tcp.snd_timeo_ms % 1000) * 1000) };
            { int r = copy_to_user(optval, tv, 16); if (r) return r; }
            { int len = 16; int r = copy_to_user(optlen, &len, sizeof(int)); if (r) return r; }
            return 0;
        }
        default: break;
        }
    } else if (level == IPPROTO_TCP) {
        switch (optname) {
        case TCP_NODELAY: val = (s->sockflags & SOCKF_NODELAY) ? 1 : 0; break;
        case TCP_CORK: val = 0; break;
        case TCP_QUICKACK: val = 1; break;
        case TCP_KEEPIDLE: val = 7200; break;
        case TCP_KEEPINTVL: val = 75; break;
        case TCP_KEEPCNT: val = 9; break;
        default: break;
        }
    }

    { int r = copy_to_user(optval, &val, sizeof(int)); if (r) return r; }
    { int len = (int)sizeof(int); int r = copy_to_user(optlen, &len, sizeof(int)); if (r) return r; }
    return 0;
}

long do_getsockname(int fd, void *addr, int *addrlen) {
    socket_t *s = sock_from_fd(fd);
    if (!s) return -ENOTSOCK;

    struct k_sockaddr_in sa;
    for (int i = 0; i < (int)sizeof(sa); i++) ((uint8_t *)&sa)[i] = 0;
    sa.sin_family = 2;
    sa.sin_port = s->local_port;
    sa.sin_addr = s->local_ip;

    { int r = copy_to_user(addr, &sa, sizeof(sa)); if (r) return r; }
    { int len = (int)sizeof(struct k_sockaddr_in); int r = copy_to_user(addrlen, &len, sizeof(int)); if (r) return r; }
    return 0;
}

long do_getpeername(int fd, void *addr, int *addrlen) {
    socket_t *s = sock_from_fd(fd);
    if (!s) return -ENOTSOCK;
    if (s->state != SOCK_CONNECTED) return -ENOTCONN;

    struct k_sockaddr_in sa;
    for (int i = 0; i < (int)sizeof(sa); i++) ((uint8_t *)&sa)[i] = 0;
    sa.sin_family = 2;
    sa.sin_port = s->remote_port;
    sa.sin_addr = s->remote_ip;

    { int r = copy_to_user(addr, &sa, sizeof(sa)); if (r) return r; }
    { int len = (int)sizeof(struct k_sockaddr_in); int r = copy_to_user(addrlen, &len, sizeof(int)); if (r) return r; }
    return 0;
}

long do_shutdown(int fd, int how) {
    process_t *p = proc_current();
    if (p) {
        fd_entry_t *fde = fd_get(&p->fds, fd);
        if (fde && fde->type == FD_UNIX_SOCK)
            return 0;
    }

    socket_t *s = sock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != SOCK_CONNECTED && s->state != SOCK_LISTENING)
        return -ENOTCONN;

    switch (how) {
    case 0 :
        s->shut_rd = 1;
        break;
    case 1 :
        s->shut_wr = 1;
        if (s->state == SOCK_CONNECTED && !s->is_dgram)
            net_tcp_close(&s->tcp);
        break;
    case 2 :
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

struct k_pollfd { int fd; short events; short revents; };
#define POLLIN  0x0001
#define POLLOUT 0x0004

long do_poll(void *fds_ptr, int nfds, int timeout) {
    if (nfds <= 0 || nfds > 256) return -EINVAL;
    struct k_pollfd kfds[256];
    { int r = copy_from_user(kfds, fds_ptr, (size_t)nfds * sizeof(struct k_pollfd)); if (r) return r; }

    int infinite = (timeout < 0);
    thread_t *ct = thread_current();
    uint64_t deadline_tsc;
    if (ct && ct->wake_at_tsc && timeout > 0) {
        deadline_tsc = ct->wake_at_tsc;
    } else {
        deadline_tsc = infinite ? 0 : timer_deadline_tsc((uint64_t)timeout);
    }

    for (;;) {
        int ready = 0;
        for (int i = 0; i < nfds; i++) {
            kfds[i].revents = 0;
            uint32_t interest = 0;
            if (kfds[i].events & POLLIN)  interest |= 0x001;
            if (kfds[i].events & POLLOUT) interest |= 0x004;
            uint32_t r_ev = fd_poll_readiness(kfds[i].fd, interest);
            if (r_ev & 0x001) kfds[i].revents |= POLLIN;
            if (r_ev & 0x004) kfds[i].revents |= POLLOUT;
            if (r_ev & 0x010) kfds[i].revents |= 0x0010;
            if (r_ev & 0x008) kfds[i].revents |= 0x0008;
            if (kfds[i].revents) ready++;
        }
        if (ready > 0) {
            copy_to_user(fds_ptr, kfds, (size_t)nfds * sizeof(struct k_pollfd));
            if (ct) { ct->wake_at_tsc = 0; ct->wake_at = 0; }
            return ready;
        }
        if (timeout == 0) return 0;
        if (!infinite && timer_tsc_now() >= deadline_tsc) {
            if (ct) { ct->wake_at_tsc = 0; ct->wake_at = 0; }
            return 0;
        }

        {
            thread_t *t = thread_current();
            if (!t) return -EFAULT;
            t->wake_at_tsc = infinite ? 0 : deadline_tsc;
            t->wake_at = 0;
            epoll_sleeper_add_ext(t);
            int timeout_ms;
            if (infinite) {
                timeout_ms = -1;
            } else {
                uint64_t now_tsc = timer_tsc_now();
                if (now_tsc >= deadline_tsc) { t->wake_at_tsc = 0; return 0; }
                timeout_ms = (int)((deadline_tsc - now_tsc) / timer_tsc_per_ms);
                if (timeout_ms <= 0) timeout_ms = 1;
            }
            event_t ev;
            { int wr = event_wait(&t->eq, &ev, timeout_ms);
            if (wr == -4) return -EINTR; }
        }
    }
}
