/* CosmoRT AF_UNIX Socket Layer — local bidirectional IPC
 *
 * Pipe-like semantics: socketpair creates two connected endpoints,
 * read drains own buffer, write fills peer's buffer.
 * Named sockets: bind to path, listen, accept, connect. */

#include "unix_socket.h"
#include "process.h"
#include "fd.h"
#include "serial.h"
#include "spinlock.h"
#include "memops.h"
#include "syscall.h"

/* ── Pool ─────────────────────────────────────── */

static unix_socket_t usock_pool[USOCK_MAX];
static spinlock_t usock_lock = SPINLOCK_INIT;

/* Validate user pointer */
static inline int user_ok(uint64_t addr, size_t len) {
    return addr < 0x800000000000ULL &&
           addr + len <= 0x800000000000ULL &&
           addr + len >= addr;
}

static unix_socket_t *usock_alloc(void) {
    uint64_t flags;
    spin_lock_irq(&usock_lock, &flags);
    for (int i = 0; i < USOCK_MAX; i++) {
        if (usock_pool[i].state == USOCK_UNUSED) {
            /* Zero the struct */
            for (int j = 0; j < (int)sizeof(unix_socket_t); j++)
                ((uint8_t *)&usock_pool[i])[j] = 0;
            usock_pool[i].state = USOCK_CREATED;
            usock_pool[i].refcount = 1;
            spin_unlock_irq(&usock_lock, flags);
            return &usock_pool[i];
        }
    }
    spin_unlock_irq(&usock_lock, flags);
    return 0;
}

unix_socket_t *usock_from_fd(int fd) {
    process_t *p = proc_current();
    if (!p) return 0;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_UNIX_SOCK) return 0;
    return (unix_socket_t *)fde->obj;
}

/* ── Refcount ─────────────────────────────────── */

void usock_incref(void *obj) {
    unix_socket_t *s = (unix_socket_t *)obj;
    if (s) __sync_add_and_fetch(&s->refcount, 1);
}

void usock_decref(void *obj) {
    unix_socket_t *s = (unix_socket_t *)obj;
    if (!s) return;
    if (__sync_sub_and_fetch(&s->refcount, 1) <= 0) {
        /* Disconnect peer */
        if (s->peer) {
            s->peer->peer = 0;
            s->peer = 0;
        }
        s->state = USOCK_UNUSED;
    }
}

/* ── Ring buffer ops ──────────────────────────── */

static int ring_write(unix_socket_t *s, const uint8_t *data, int len) {
    int space = USOCK_BUF_SIZE - s->count;
    int n = len < space ? len : space;
    for (int i = 0; i < n; i++) {
        s->buf[s->tail] = data[i];
        s->tail = (s->tail + 1) % USOCK_BUF_SIZE;
    }
    s->count += n;
    return n;
}

static int ring_read(unix_socket_t *s, uint8_t *data, int len) {
    int avail = s->count;
    int n = len < avail ? len : avail;
    for (int i = 0; i < n; i++) {
        data[i] = s->buf[s->head];
        s->head = (s->head + 1) % USOCK_BUF_SIZE;
    }
    s->count -= n;
    return n;
}

/* ── socket(AF_UNIX, ...) ─────────────────────── */

long usock_socket(int type) {
    int base_type = type & 0xF;
    if (base_type != 1 /* SOCK_STREAM */) return -EPROTONOSUPPORT;

    unix_socket_t *s = usock_alloc();
    if (!s) return -EMFILE;

    process_t *p = proc_current();
    if (!p) { s->state = USOCK_UNUSED; return -EFAULT; }

    int fd_flags = 0x02; /* O_RDWR */
    if (type & 0x80000) fd_flags |= 0x80000;  /* SOCK_CLOEXEC */
    if (type & 0x800)   fd_flags |= 0x800;    /* SOCK_NONBLOCK */
    s->flags = type & (0x80000 | 0x800);

    int fd = fd_alloc(&p->fds, FD_UNIX_SOCK, s, fd_flags);
    if (fd < 0) { s->state = USOCK_UNUSED; return -EMFILE; }
    return fd;
}

/* ── socketpair(AF_UNIX, SOCK_STREAM, 0, sv[2]) ─ */

long usock_socketpair(int type, int *sv) {
    if (!user_ok((uint64_t)sv, 2 * sizeof(int))) return -EFAULT;

    int base_type = type & 0xF;
    if (base_type != 1 /* SOCK_STREAM */) return -EPROTONOSUPPORT;

    unix_socket_t *a = usock_alloc();
    if (!a) return -EMFILE;
    unix_socket_t *b = usock_alloc();
    if (!b) { a->state = USOCK_UNUSED; return -EMFILE; }

    /* Cross-connect */
    a->peer = b;
    b->peer = a;
    a->state = USOCK_CONNECTED;
    b->state = USOCK_CONNECTED;
    a->flags = type & (0x80000 | 0x800);
    b->flags = type & (0x80000 | 0x800);

    process_t *p = proc_current();
    if (!p) { a->state = USOCK_UNUSED; b->state = USOCK_UNUSED; return -EFAULT; }

    int fd_flags = 0x02; /* O_RDWR */
    if (type & 0x80000) fd_flags |= 0x80000;
    if (type & 0x800)   fd_flags |= 0x800;

    int fd0 = fd_alloc(&p->fds, FD_UNIX_SOCK, a, fd_flags);
    if (fd0 < 0) { a->state = USOCK_UNUSED; b->state = USOCK_UNUSED; return -EMFILE; }

    int fd1 = fd_alloc(&p->fds, FD_UNIX_SOCK, b, fd_flags);
    if (fd1 < 0) {
        fd_close(&p->fds, fd0);
        a->state = USOCK_UNUSED;
        b->state = USOCK_UNUSED;
        return -EMFILE;
    }

    /* Copy to user */
    int k_sv[2] = { fd0, fd1 };
    kmemcpy(sv, k_sv, sizeof(k_sv));
    return 0;
}

/* ── bind ─────────────────────────────────────── */

long usock_bind(int fd, const struct k_sockaddr_un *addr, int addrlen) {
    if (addrlen < 3) return -EINVAL; /* at least family + 1 char */
    if (!user_ok((uint64_t)addr, (size_t)addrlen)) return -EFAULT;

    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != USOCK_CREATED) return -EINVAL;

    struct k_sockaddr_un k_addr;
    int copy_len = addrlen < (int)sizeof(k_addr) ? addrlen : (int)sizeof(k_addr);
    kmemcpy(&k_addr, addr, (size_t)copy_len);

    if (k_addr.sun_family != 1 /* AF_UNIX */) return -EINVAL;

    /* Copy path */
    int path_len = copy_len - 2; /* minus sun_family */
    if (path_len <= 0 || path_len >= 108) return -EINVAL;
    kmemcpy(s->path, k_addr.sun_path, (size_t)path_len);
    s->path[path_len] = '\0';

    /* Check for name collision */
    uint64_t flags;
    spin_lock_irq(&usock_lock, &flags);
    for (int i = 0; i < USOCK_MAX; i++) {
        if (&usock_pool[i] == s) continue;
        if (usock_pool[i].state != USOCK_UNUSED && usock_pool[i].path[0]) {
            /* Compare paths */
            int match = 1;
            for (int j = 0; j < 108; j++) {
                if (usock_pool[i].path[j] != s->path[j]) { match = 0; break; }
                if (s->path[j] == '\0') break;
            }
            if (match) {
                spin_unlock_irq(&usock_lock, flags);
                return -EADDRINUSE;
            }
        }
    }
    spin_unlock_irq(&usock_lock, flags);
    return 0;
}

/* ── listen ───────────────────────────────────── */

long usock_listen(int fd, int backlog) {
    (void)backlog;
    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != USOCK_CREATED) return -EINVAL;
    if (!s->path[0]) return -EINVAL; /* must be bound */
    s->state = USOCK_LISTENING;
    return 0;
}

/* ── accept4 ──────────────────────────────────── */

long usock_accept4(int fd, void *addr, int *addrlen, int flags) {
    (void)addr; (void)addrlen;
    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != USOCK_LISTENING) return -EINVAL;

    /* Check backlog */
    if (s->backlog_count == 0) return -EAGAIN;

    /* Dequeue first pending connection */
    unix_socket_t *client = s->backlog[0];
    for (int i = 1; i < s->backlog_count; i++)
        s->backlog[i - 1] = s->backlog[i];
    s->backlog_count--;

    /* Create server-side endpoint */
    unix_socket_t *server = usock_alloc();
    if (!server) return -EMFILE;

    /* Connect */
    server->peer = client;
    client->peer = server;
    server->state = USOCK_CONNECTED;
    client->state = USOCK_CONNECTED;

    process_t *p = proc_current();
    if (!p) { server->state = USOCK_UNUSED; return -EFAULT; }

    int fd_flags = 0x02;
    if (flags & 0x80000) fd_flags |= 0x80000;  /* SOCK_CLOEXEC */
    if (flags & 0x800)   fd_flags |= 0x800;    /* SOCK_NONBLOCK */

    int new_fd = fd_alloc(&p->fds, FD_UNIX_SOCK, server, fd_flags);
    if (new_fd < 0) { server->state = USOCK_UNUSED; return -EMFILE; }
    return new_fd;
}

/* ── connect ──────────────────────────────────── */

long usock_connect(int fd, const struct k_sockaddr_un *addr, int addrlen) {
    if (addrlen < 3) return -EINVAL;
    if (!user_ok((uint64_t)addr, (size_t)addrlen)) return -EFAULT;

    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != USOCK_CREATED) return -EISCONN;

    struct k_sockaddr_un k_addr;
    int copy_len = addrlen < (int)sizeof(k_addr) ? addrlen : (int)sizeof(k_addr);
    kmemcpy(&k_addr, addr, (size_t)copy_len);

    if (k_addr.sun_family != 1 /* AF_UNIX */) return -EINVAL;

    int path_len = copy_len - 2;
    if (path_len <= 0 || path_len >= 108) return -EINVAL;
    char target[108];
    kmemcpy(target, k_addr.sun_path, (size_t)path_len);
    target[path_len] = '\0';

    /* Find listening socket with matching path */
    uint64_t flags;
    spin_lock_irq(&usock_lock, &flags);
    unix_socket_t *listener = 0;
    for (int i = 0; i < USOCK_MAX; i++) {
        if (usock_pool[i].state != USOCK_LISTENING) continue;
        int match = 1;
        for (int j = 0; j < 108; j++) {
            if (usock_pool[i].path[j] != target[j]) { match = 0; break; }
            if (target[j] == '\0') break;
        }
        if (match) { listener = &usock_pool[i]; break; }
    }
    if (!listener) {
        spin_unlock_irq(&usock_lock, flags);
        return -ECONNREFUSED;
    }
    if (listener->backlog_count >= USOCK_BACKLOG_MAX) {
        spin_unlock_irq(&usock_lock, flags);
        return -EAGAIN;
    }
    /* Enqueue in listener's backlog */
    listener->backlog[listener->backlog_count++] = s;
    spin_unlock_irq(&usock_lock, flags);

    /* Connection completes when accept() dequeues us.
     * For simplicity (no blocking connect), return 0 — client
     * transitions to CONNECTED when accept creates the peer. */
    return 0;
}

/* ── read/write ───────────────────────────────── */

long usock_read(int fd, void *buf, long count) {
    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != USOCK_CONNECTED) return -ENOTCONN;

    if (s->count == 0) {
        if (!s->peer) return 0; /* EOF: peer closed */
        return -EAGAIN;
    }

    int n = ring_read(s, (uint8_t *)buf, (int)count);

    /* Wake epoll/poll */
    extern void epoll_wake_all(void);
    epoll_wake_all();

    return (long)n;
}

long usock_write(int fd, const void *buf, long count) {
    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != USOCK_CONNECTED || !s->peer) return -EPIPE;

    /* Write into peer's receive buffer */
    int n = ring_write(s->peer, (const uint8_t *)buf, (int)count);
    if (n == 0) return -EAGAIN; /* peer buffer full */

    /* Wake epoll/poll */
    extern void epoll_wake_all(void);
    epoll_wake_all();

    return (long)n;
}

/* ── close ────────────────────────────────────── */

long usock_close(int fd) {
    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;

    process_t *p = proc_current();
    if (p) fd_close(&p->fds, fd);

    usock_decref(s);

    /* Wake epoll/poll — peer may need POLLHUP */
    extern void epoll_wake_all(void);
    epoll_wake_all();

    return 0;
}

/* ── sendmsg/recvmsg ──────────────────────────── */

/* Kernel-internal msghdr layout (matches Linux x86_64) */
struct k_msghdr {
    void       *msg_name;
    uint32_t    msg_namelen;
    uint32_t    _pad0;
    struct iovec *msg_iov;
    uint64_t    msg_iovlen;
    void       *msg_control;
    uint64_t    msg_controllen;
    int         msg_flags;
    int         _pad1;
};

struct iovec { const void *iov_base; size_t iov_len; };

long usock_sendmsg(int fd, const void *msg_ptr, int flags) {
    (void)flags;
    if (!user_ok((uint64_t)msg_ptr, sizeof(struct k_msghdr))) return -EFAULT;

    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != USOCK_CONNECTED || !s->peer) return -EPIPE;

    /* Copy msghdr to kernel */
    struct k_msghdr kmsg;
    kmemcpy(&kmsg, msg_ptr, sizeof(kmsg));

    if (kmsg.msg_iovlen == 0) return 0;
    if (kmsg.msg_iovlen > 16) return -EMSGSIZE;
    if (!user_ok((uint64_t)kmsg.msg_iov, kmsg.msg_iovlen * sizeof(struct iovec)))
        return -EFAULT;

    /* Copy iov array */
    struct iovec k_iov[16];
    kmemcpy(k_iov, kmsg.msg_iov, kmsg.msg_iovlen * sizeof(struct iovec));

    long total = 0;
    for (uint64_t i = 0; i < kmsg.msg_iovlen; i++) {
        if (!k_iov[i].iov_len) continue;
        if (!user_ok((uint64_t)k_iov[i].iov_base, k_iov[i].iov_len)) return -EFAULT;

        /* Bounce through kernel buffer to prevent TOCTOU */
        uint8_t kbuf[1024];
        uint64_t remaining = k_iov[i].iov_len;
        const uint8_t *src = (const uint8_t *)k_iov[i].iov_base;
        while (remaining > 0) {
            uint64_t chunk = remaining > sizeof(kbuf) ? sizeof(kbuf) : remaining;
            kmemcpy(kbuf, src, (size_t)chunk);
            int w = ring_write(s->peer, kbuf, (int)chunk);
            if (w <= 0) {
                if (total > 0) goto done;
                return -EAGAIN;
            }
            total += w;
            src += w;
            remaining -= (uint64_t)w;
            if (w < (int)chunk) break; /* buffer full */
        }
    }
done:
    if (total > 0) {
        extern void epoll_wake_all(void);
        epoll_wake_all();
    }
    return total;
}

long usock_recvmsg(int fd, void *msg_ptr, int flags) {
    (void)flags;
    if (!user_ok((uint64_t)msg_ptr, sizeof(struct k_msghdr))) return -EFAULT;

    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != USOCK_CONNECTED) return -ENOTCONN;

    /* Copy msghdr to kernel */
    struct k_msghdr kmsg;
    kmemcpy(&kmsg, msg_ptr, sizeof(kmsg));

    if (kmsg.msg_iovlen == 0) return 0;
    if (kmsg.msg_iovlen > 16) return -EINVAL;
    if (!user_ok((uint64_t)kmsg.msg_iov, kmsg.msg_iovlen * sizeof(struct iovec)))
        return -EFAULT;

    /* Copy iov array */
    struct iovec k_iov[16];
    kmemcpy(k_iov, kmsg.msg_iov, kmsg.msg_iovlen * sizeof(struct iovec));

    if (s->count == 0) {
        if (!s->peer) return 0; /* EOF */
        return -EAGAIN;
    }

    long total = 0;
    for (uint64_t i = 0; i < kmsg.msg_iovlen; i++) {
        if (!k_iov[i].iov_len) continue;
        if (!user_ok((uint64_t)k_iov[i].iov_base, k_iov[i].iov_len)) return -EFAULT;

        uint8_t kbuf[1024];
        uint64_t remaining = k_iov[i].iov_len;
        uint8_t *dst = (uint8_t *)k_iov[i].iov_base;
        while (remaining > 0 && s->count > 0) {
            uint64_t chunk = remaining > sizeof(kbuf) ? sizeof(kbuf) : remaining;
            int r = ring_read(s, kbuf, (int)chunk);
            if (r <= 0) break;
            kmemcpy(dst, kbuf, (size_t)r);
            total += r;
            dst += r;
            remaining -= (uint64_t)r;
        }
        if (s->count == 0) break;
    }

    /* Zero out msg_controllen — no ancillary data */
    kmsg.msg_controllen = 0;
    kmsg.msg_flags = 0;
    kmemcpy(msg_ptr, &kmsg, sizeof(kmsg));

    if (total > 0) {
        extern void epoll_wake_all(void);
        epoll_wake_all();
    }
    return total;
}
