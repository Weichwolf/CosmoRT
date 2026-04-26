/* CosmoRT AF_UNIX Socket Layer — local bidirectional IPC
 *
 * Pipe-like semantics: socketpair creates two connected endpoints,
 * read drains own buffer, write fills peer's buffer.
 * Named sockets: bind to path, listen, accept, connect. */

#include "net/unix_socket.h"
#include "net/net_ns.h"
#include "proc/process.h"
#include "event/fd.h"
#include "hw/serial.h"
#include "spinlock.h"
#include "memops.h"
#include "sys/syscall.h"
#include "cosmort.h"
#include "arch/arch.h"
#include "core/waitqueue.h"
#include "mm/slab.h"
#include "fs/vfs.h"

#define USOCK_PATH_MAX_RESOLVE 4096
extern int resolve_path(const char *path, char *out, int outsize);

extern long send_sigpipe(void);

/* ── Slab + active list ───────────────────────── */

static slab_t usock_slab;
static int usock_slab_inited;
static unix_socket_t *usock_active_head;
static spinlock_t usock_lock = SPINLOCK_INIT;

/* User-pointer validation + copy helpers */
#include "uaccess.h"

static void usock_slab_ensure(void) {
    if (__sync_bool_compare_and_swap(&usock_slab_inited, 0, 1))
        slab_init_dynamic(&usock_slab, (int)sizeof(unix_socket_t), 0);
}

/* Caller holds usock_lock */
static void usock_list_add(unix_socket_t *s) {
    s->prev_active = 0;
    s->next_active = usock_active_head;
    if (usock_active_head) usock_active_head->prev_active = s;
    usock_active_head = s;
}

/* Caller holds usock_lock */
static void usock_list_del(unix_socket_t *s) {
    if (s->prev_active) s->prev_active->next_active = s->next_active;
    else                usock_active_head = s->next_active;
    if (s->next_active) s->next_active->prev_active = s->prev_active;
    s->next_active = 0;
    s->prev_active = 0;
}

static unix_socket_t *usock_alloc(void) {
    usock_slab_ensure();
    unix_socket_t *s = (unix_socket_t *)slab_alloc(&usock_slab);
    if (!s) return 0;
    /* slab_alloc zeroes */
    s->state = USOCK_CREATED;
    s->refcount = 1;
    s->ns_id = net_ns_current()->ns_id;
    init_waitqueue_head(&s->read_wq);
    init_waitqueue_head(&s->write_wq);
    init_waitqueue_head(&s->accept_wq);
    init_waitqueue_head(&s->connect_wq);
    uint64_t flags;
    spin_lock_irq(&usock_lock, &flags);
    usock_list_add(s);
    spin_unlock_irq(&usock_lock, flags);
    return s;
}

static void usock_release(unix_socket_t *s) {
    uint64_t flags;
    spin_lock_irq(&usock_lock, &flags);
    usock_list_del(s);
    spin_unlock_irq(&usock_lock, flags);
    slab_free(&usock_slab, s);
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
        /* If we are still queued in a listener's backlog (close before
         * accept), unlink first so the listener doesn't dereference us
         * after slab_free. */
        if (s->backlog_owner) {
            uint64_t bf;
            spin_lock_irq(&usock_lock, &bf);
            unix_socket_t *L = s->backlog_owner;
            if (L) {
                unix_socket_t **pp = &L->backlog_head;
                unix_socket_t *prev = 0;
                while (*pp && *pp != s) { prev = *pp; pp = &(*pp)->backlog_next; }
                if (*pp == s) {
                    *pp = s->backlog_next;
                    if (L->backlog_tail == s) L->backlog_tail = prev;
                    L->backlog_count--;
                }
                s->backlog_next = 0;
                s->backlog_owner = 0;
            }
            spin_unlock_irq(&usock_lock, bf);
        }
        /* Detach from peer — readers on peer wake to see EOF (no peer) */
        unix_socket_t *peer = 0;
        if (s->peer) {
            uint64_t irqf;
            spin_lock_irq(&usock_lock, &irqf);
            peer = s->peer;
            if (peer) peer->peer = 0;
            s->peer = 0;
            spin_unlock_irq(&usock_lock, irqf);
        }

        /* Drain pending backlog: each enqueued client has a thread blocked
         * in usock_connect waiting for accept(). Wake them via their own
         * connect_wq so they re-check state and unwind cleanly. The client
         * sockets keep their own refcounts — only the queue link is dropped. */
        unix_socket_t *drained_head = 0;
        if (s->backlog_head) {
            uint64_t irqf;
            spin_lock_irq(&usock_lock, &irqf);
            drained_head = s->backlog_head;
            s->backlog_head = 0;
            s->backlog_tail = 0;
            s->backlog_count = 0;
            spin_unlock_irq(&usock_lock, irqf);
        }
        while (drained_head) {
            unix_socket_t *next = drained_head->backlog_next;
            drained_head->backlog_next = 0;
            drained_head->backlog_owner = 0;
            wake_up_all(&drained_head->connect_wq);
            drained_head = next;
        }

        /* Broadcast EOF/HUP to peer-side blocked readers/writers. */
        if (peer) {
            wake_up_all(&peer->read_wq);
            wake_up_all(&peer->write_wq);
        }
        /* Wake any acceptor blocked on this listener — re-check finds
         * USOCK_LISTENING dropped (refcount reached 0 → socket dies). */
        wake_up_all(&s->accept_wq);
        usock_release(s);
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
    /* SOCK_STREAM=1 and SOCK_SEQPACKET=5: both are reliable connection-oriented.
     * We treat SEQPACKET like STREAM — message boundaries are not enforced but
     * bind/listen/accept/connect/send/recv all work per-Linux semantics. */
    if (base_type != 1 && base_type != 5) return -EPROTONOSUPPORT;

    unix_socket_t *s = usock_alloc();
    if (!s) return -EMFILE;

    process_t *p = proc_current();
    if (!p) { usock_release(s); return -EFAULT; }

    int fd_flags = 0x02; /* O_RDWR */
    if (type & 0x80000) fd_flags |= 0x80000;  /* SOCK_CLOEXEC */
    if (type & 0x800)   fd_flags |= 0x800;    /* SOCK_NONBLOCK */
    s->flags = type & (0x80000 | 0x800);

    int fd = fd_alloc(&p->fds, FD_UNIX_SOCK, s, fd_flags);
    if (fd < 0) { usock_release(s); return -EMFILE; }
    return fd;
}

/* ── socketpair(AF_UNIX, SOCK_STREAM, 0, sv[2]) ─ */

long usock_socketpair(int type, int *sv) {
    if (!user_ok((uint64_t)sv, 2 * sizeof(int))) return -EFAULT; /* validated early, copy_to_user below */

    int base_type = type & 0xF;
    if (base_type != 1 && base_type != 5) return -EPROTONOSUPPORT;

    unix_socket_t *a = usock_alloc();
    if (!a) return -EMFILE;
    unix_socket_t *b = usock_alloc();
    if (!b) { usock_release(a); return -EMFILE; }

    /* Cross-connect */
    a->peer = b;
    b->peer = a;
    a->state = USOCK_CONNECTED;
    b->state = USOCK_CONNECTED;
    a->flags = type & (0x80000 | 0x800);
    b->flags = type & (0x80000 | 0x800);

    process_t *p = proc_current();
    if (!p) { usock_release(a); usock_release(b); return -EFAULT; }

    int fd_flags = 0x02; /* O_RDWR */
    if (type & 0x80000) fd_flags |= 0x80000;
    if (type & 0x800)   fd_flags |= 0x800;

    int fd0 = fd_alloc(&p->fds, FD_UNIX_SOCK, a, fd_flags);
    if (fd0 < 0) { usock_release(a); usock_release(b); return -EMFILE; }

    int fd1 = fd_alloc(&p->fds, FD_UNIX_SOCK, b, fd_flags);
    if (fd1 < 0) {
        fd_close(&p->fds, fd0);
        usock_release(a);
        usock_release(b);
        return -EMFILE;
    }

    /* Copy to user */
    int k_sv[2] = { fd0, fd1 };
    copy_to_user(sv, k_sv, sizeof(k_sv)); /* user_ok checked at entry */
    return 0;
}

/* ── bind ─────────────────────────────────────── */

long usock_bind(int fd, const struct k_sockaddr_un *addr, int addrlen) {
    if (addrlen < 3) return -EINVAL; /* at least family + 1 char */

    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != USOCK_CREATED) return -EINVAL;
    if (s->path_len > 0) return -EINVAL; /* already bound — no rebind */

    struct k_sockaddr_un k_addr;
    int copy_len = addrlen < (int)sizeof(k_addr) ? addrlen : (int)sizeof(k_addr);
    { int r = copy_from_user(&k_addr, addr, (size_t)copy_len); if (r) return r; }

    if (k_addr.sun_family != 1 /* AF_UNIX */) return -EINVAL;

    int max_path = copy_len - 2; /* minus sun_family */
    if (max_path <= 0) return -EINVAL;
    if (max_path > 108) max_path = 108;

    char new_path[108];
    int new_len;
    int is_abstract = (k_addr.sun_path[0] == '\0');

    if (is_abstract) {
        /* Abstract namespace: full addrlen-2 bytes, including leading NUL. */
        new_len = max_path;
        for (int i = 0; i < new_len; i++) new_path[i] = k_addr.sun_path[i];
    } else {
        /* Pathname: C-string up to first NUL. */
        new_len = 0;
        for (int i = 0; i < max_path; i++) {
            if (k_addr.sun_path[i] == '\0') break;
            new_path[i] = k_addr.sun_path[i];
            new_len++;
        }
        if (new_len == 0) return -EINVAL;

        /* Pathname socket: verify parent directory is a dir. */
        char cpath[109];
        for (int i = 0; i < new_len; i++) cpath[i] = new_path[i];
        cpath[new_len] = '\0';
        int slash = -1;
        for (int i = 0; cpath[i]; i++) if (cpath[i] == '/') slash = i;
        if (slash > 0) {
            char parent[USOCK_PATH_MAX_RESOLVE];
            char resolved[USOCK_PATH_MAX_RESOLVE];
            int i = 0;
            for (; i < slash && i < USOCK_PATH_MAX_RESOLVE - 1; i++) parent[i] = cpath[i];
            parent[i] = '\0';
            if (resolve_path(parent, resolved, USOCK_PATH_MAX_RESOLVE) == 0) {
                struct k_stat st;
                int sr = vfs_stat(resolved, &st);
                if (sr == 0 && (st.st_mode & S_IFMT) != S_IFDIR)
                    return -ENOTDIR;
            }
        }
    }

    uint64_t flags;
    spin_lock_irq(&usock_lock, &flags);
    for (unix_socket_t *o = usock_active_head; o; o = o->next_active) {
        if (o == s || o->path_len == 0) continue;
        if (o->path_len != new_len) continue;
        /* Abstract sockets only collide inside the same NS; pathname
         * sockets are global because the underlying VFS path is shared. */
        if (is_abstract && o->ns_id != s->ns_id) continue;
        int match = 1;
        for (int j = 0; j < new_len; j++) {
            if (o->path[j] != new_path[j]) { match = 0; break; }
        }
        if (match) {
            spin_unlock_irq(&usock_lock, flags);
            return -EADDRINUSE;
        }
    }
    for (int i = 0; i < new_len; i++) s->path[i] = new_path[i];
    s->path_len = new_len;
    spin_unlock_irq(&usock_lock, flags);

    /* Pathname sockets create a filesystem node. Linux uses S_IFSOCK;
     * we use a regular file so the path is visible to stat/unlink — our
     * VFS does not distinguish socket inodes, and the socket itself is
     * a kernel object looked up via usock_active_head. */
    if (!is_abstract) {
        char cpath[109];
        for (int i = 0; i < new_len; i++) cpath[i] = new_path[i];
        cpath[new_len] = '\0';
        /* vfs_open expects absolute path — resolve CWD + path for relative. */
        char abspath[USOCK_PATH_MAX_RESOLVE];
        const char *target = cpath;
        if (cpath[0] != '/' &&
            resolve_path(cpath, abspath, USOCK_PATH_MAX_RESOLVE) == 0)
            target = abspath;
        /* O_EXCL so a pre-existing file turns into EADDRINUSE (Linux). */
        int nfd = vfs_open(target, O_CREAT | O_EXCL | O_WRONLY, 0666);
        if (nfd < 0) {
            uint64_t f2;
            spin_lock_irq(&usock_lock, &f2);
            for (int i = 0; i < new_len; i++) s->path[i] = 0;
            s->path_len = 0;
            spin_unlock_irq(&usock_lock, f2);
            if (nfd == -EEXIST) return -EADDRINUSE;
            return nfd;
        }
        vfs_close(nfd);
    }
    return 0;
}

/* ── listen ───────────────────────────────────── */

long usock_listen(int fd, int backlog) {
    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;
    /* Linux: re-listen on a LISTENING socket only updates the backlog cap. */
    if (s->state != USOCK_CREATED && s->state != USOCK_LISTENING) return -EINVAL;
    if (s->path_len == 0) return -EINVAL; /* must be bound */
    /* Linux: negative is silently clamped to SOMAXCONN; 0 stays 0 (rejects
     * all connects). Positive values are clamped to the system cap. */
    int cap = backlog;
    if (cap < 0) cap = USOCK_SOMAXCONN;
    if (cap > USOCK_SOMAXCONN) cap = USOCK_SOMAXCONN;
    s->backlog_cap = cap;
    s->state = USOCK_LISTENING;
    return 0;
}

/* ── accept4 ──────────────────────────────────── */

long usock_accept4(int fd, void *addr, int *addrlen, int flags) {
    (void)addr; (void)addrlen;
    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != USOCK_LISTENING) return -EINVAL;

    /* Block until a connection arrives (unless O_NONBLOCK). prepare_to_wait
     * serialises state-transition with usock_connect's wake_up — re-check
     * under usock_lock catches any enqueue between sleep prep and schedule. */
    int nonblock = 0;
    { process_t *rp = proc_current();
      if (rp) { fd_entry_t *rf = fd_get(&rp->fds, fd);
                 if (rf && (rf->flags & 0x800)) nonblock = 1; } }

    unix_socket_t *client = 0;
    {
        DEFINE_WAIT(wait);
        long rc = 0;
        for (;;) {
            prepare_to_wait(&s->accept_wq, &wait, /*THREAD_BLOCKED*/ 3);

            uint64_t lf;
            spin_lock_irq(&usock_lock, &lf);
            if (s->backlog_head) {
                client = s->backlog_head;
                s->backlog_head = client->backlog_next;
                if (!s->backlog_head) s->backlog_tail = 0;
                client->backlog_next = 0;
                client->backlog_owner = 0;
                s->backlog_count--;
                spin_unlock_irq(&usock_lock, lf);
                break;
            }
            spin_unlock_irq(&usock_lock, lf);

            if (nonblock) { rc = -EAGAIN; break; }
            if (signal_deliverable()) { rc = -ERESTARTSYS; break; }
            schedule();
        }
        finish_wait(&s->accept_wq, &wait);
        if (rc < 0) return rc;
    }
    if (!client) return -EAGAIN;

    /* Create server-side endpoint */
    unix_socket_t *server = usock_alloc();
    if (!server) return -EMFILE;

    /* Connect */
    server->peer = client;
    client->peer = server;
    server->state = USOCK_CONNECTED;
    client->state = USOCK_CONNECTED;
    /* Wake any thread blocked in usock_connect on the client side. */
    wake_up_all(&client->connect_wq);

    process_t *p = proc_current();
    if (!p) { usock_release(server); return -EFAULT; }

    int fd_flags = 0x02;
    if (flags & 0x80000) fd_flags |= 0x80000;  /* SOCK_CLOEXEC */
    if (flags & 0x800)   fd_flags |= 0x800;    /* SOCK_NONBLOCK */

    int new_fd = fd_alloc(&p->fds, FD_UNIX_SOCK, server, fd_flags);
    if (new_fd < 0) { usock_release(server); return -EMFILE; }
    return new_fd;
}

/* ── connect ──────────────────────────────────── */

long usock_connect(int fd, const struct k_sockaddr_un *addr, int addrlen) {
    if (addrlen < 3) return -EINVAL;

    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != USOCK_CREATED) return -EISCONN;

    struct k_sockaddr_un k_addr;
    int copy_len = addrlen < (int)sizeof(k_addr) ? addrlen : (int)sizeof(k_addr);
    { int r = copy_from_user(&k_addr, addr, (size_t)copy_len); if (r) return r; }

    if (k_addr.sun_family != 1 /* AF_UNIX */) return -EINVAL;

    int raw_len = copy_len - 2;
    if (raw_len <= 0 || raw_len > 108) return -EINVAL;
    int is_abstract = (k_addr.sun_path[0] == '\0');
    char target[108];
    int target_len;
    if (is_abstract) {
        target_len = raw_len;
        for (int i = 0; i < target_len; i++) target[i] = k_addr.sun_path[i];
    } else {
        target_len = 0;
        for (int i = 0; i < raw_len; i++) {
            if (k_addr.sun_path[i] == '\0') break;
            target[i] = k_addr.sun_path[i];
            target_len++;
        }
        if (target_len == 0) return -EINVAL;
    }

    /* Find listening socket with matching path via active list. Abstract
     * sockets are NS-scoped; pathname sockets are global (filesystem). */
    uint64_t flags;
    spin_lock_irq(&usock_lock, &flags);
    unix_socket_t *listener = 0;
    for (unix_socket_t *o = usock_active_head; o; o = o->next_active) {
        if (o->state != USOCK_LISTENING) continue;
        if (o->path_len != target_len) continue;
        if (is_abstract && o->ns_id != s->ns_id) continue;
        int match = 1;
        for (int j = 0; j < target_len; j++) {
            if (o->path[j] != target[j]) { match = 0; break; }
        }
        if (match) { listener = o; break; }
    }
    if (!listener) {
        spin_unlock_irq(&usock_lock, flags);
        return -ECONNREFUSED;
    }
    if (listener->backlog_count >= listener->backlog_cap) {
        spin_unlock_irq(&usock_lock, flags);
        /* Linux: a refused-by-full-backlog returns -ECONNREFUSED on UNIX
         * sockets (different from TCP, which is -ECONNREFUSED only when
         * no listener exists). We follow the existing return contract. */
        return -EAGAIN;
    }
    /* Enqueue at tail (FIFO). Caller-side socket links via backlog_next;
     * accept() pops from head. backlog_owner lets close() unlink the
     * client if it dies before being accepted. */
    s->backlog_next = 0;
    s->backlog_owner = listener;
    if (listener->backlog_tail)
        listener->backlog_tail->backlog_next = s;
    else
        listener->backlog_head = s;
    listener->backlog_tail = s;
    listener->backlog_count++;
    spin_unlock_irq(&usock_lock, flags);
    /* Wake any acceptor blocked on the listener; multiple acceptors are
     * legal (e.g. dup'd listener fd in two threads). Acceptor's wq wake
     * propagates to ep->wq via ep_poll_callback for any registered epitem. */
    wake_up_all(&listener->accept_wq);

    /* Block until accept() completes the handshake (transitions us to
     * USOCK_CONNECTED). Without this, write() after connect() can race
     * and see USOCK_CREATED -> SIGPIPE. O_NONBLOCK returns EINPROGRESS. */
    int cnonblock = (s->flags & 0x800) ? 1 : 0;
    if (!cnonblock) {
        DEFINE_WAIT(wait);
        long rc = 0;
        for (;;) {
            prepare_to_wait(&s->connect_wq, &wait, /*THREAD_BLOCKED*/ 3);
            if (s->state == USOCK_CONNECTED) break;
            if (signal_deliverable()) { rc = -ERESTARTSYS; break; }
            schedule();
        }
        finish_wait(&s->connect_wq, &wait);
        if (rc < 0) return rc;
    }

    /* Connection completes when accept() dequeues us. */
    return 0;
}

/* ── read/write ───────────────────────────────── */

long usock_read(int fd, void *buf, long count) {
    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != USOCK_CONNECTED) return -ENOTCONN;

    uint64_t irqf;
    spin_lock_irq(&usock_lock, &irqf);
    if (s->count == 0) {
        int eof = !s->peer;
        spin_unlock_irq(&usock_lock, irqf);
        return eof ? 0 : -EAGAIN;
    }

    int n = ring_read(s, (uint8_t *)buf, (int)count);
    unix_socket_t *peer = s->peer;
    spin_unlock_irq(&usock_lock, irqf);

    /* Drained s->buf → wake any writer blocked because peer's buf (== s) was
     * full. Writer parks on its own write_wq; peer is the writer side. */
    if (peer) wake_up(&peer->write_wq);
    return (long)n;
}

/* Blocking unix socket read: called when usock_read returned -EAGAIN
 * and O_NONBLOCK is not set. prepare_to_wait/finish_wait closes the wakeup
 * race: state=BLOCKED is set under read_wq->lock before re-checking
 * s->count/s->peer; any usock_write between check and schedule must take
 * read_wq->lock to wake us, so we never miss a wakeup. */
long usock_read_blocking(unix_socket_t *s, void *buf, long count) {
    DEFINE_WAIT(wait);
    long rc = 0;
    for (;;) {
        prepare_to_wait(&s->read_wq, &wait, /*THREAD_BLOCKED*/ 3);

        uint64_t irqf;
        spin_lock_irq(&usock_lock, &irqf);
        if (s->count > 0) {
            int n = ring_read(s, (uint8_t *)buf, (int)count);
            unix_socket_t *peer = s->peer;
            spin_unlock_irq(&usock_lock, irqf);
            if (peer) wake_up(&peer->write_wq);
            rc = (long)n;
            break;
        }
        if (!s->peer) {
            spin_unlock_irq(&usock_lock, irqf);
            rc = 0; /* EOF: peer closed */
            break;
        }
        spin_unlock_irq(&usock_lock, irqf);

        if (signal_deliverable()) { rc = -ERESTARTSYS; break; }
        schedule();
    }
    finish_wait(&s->read_wq, &wait);
    return rc;
}

long usock_write(int fd, const void *buf, long count) {
    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != USOCK_CONNECTED) return send_sigpipe();

    /* Snapshot peer under lock — peer can be NULLed by concurrent close */
    uint64_t irqf;
    spin_lock_irq(&usock_lock, &irqf);
    unix_socket_t *peer = s->peer;
    if (!peer) { spin_unlock_irq(&usock_lock, irqf); return send_sigpipe(); }

    /* Write into peer's receive buffer (under lock — protects ring) */
    int n = ring_write(peer, (const uint8_t *)buf, (int)count);
    if (n == 0) { spin_unlock_irq(&usock_lock, irqf); return -EAGAIN; }
    spin_unlock_irq(&usock_lock, irqf);

    /* Wake reader(s) blocked on peer's read_wq — data just landed in peer->buf.
     * Registered ep_poll_callback fires from read_wq into ep->wq. */
    wake_up(&peer->read_wq);
    return (long)n;
}

/* Blocking write: retry until at least 1 byte written or peer closes.
 * Park on s->write_wq; the partner's reader wakes us via wake_up(&peer
 * ->write_wq) after draining its (== our peer's) buffer.
 *
 * prepare_to_wait + re-check-under-usock_lock closes the missed-wakeup
 * race: any usock_read or usock_decref between condition-check and
 * schedule() must take write_wq->lock to wake us, so the wake cannot
 * be lost. */
long usock_write_blocking(unix_socket_t *s, const void *buf, long count) {
    DEFINE_WAIT(wait);
    long rc = 0;
    for (;;) {
        prepare_to_wait(&s->write_wq, &wait, /*THREAD_BLOCKED*/ 3);

        uint64_t irqf;
        spin_lock_irq(&usock_lock, &irqf);
        unix_socket_t *peer = s->peer;
        if (!peer) { spin_unlock_irq(&usock_lock, irqf); rc = send_sigpipe(); break; }

        int n = ring_write(peer, (const uint8_t *)buf, (int)count);
        if (n > 0) {
            spin_unlock_irq(&usock_lock, irqf);
            wake_up(&peer->read_wq);
            rc = (long)n;
            break;
        }
        spin_unlock_irq(&usock_lock, irqf);

        if (signal_deliverable()) { rc = -ERESTARTSYS; break; }
        schedule();
    }
    finish_wait(&s->write_wq, &wait);
    return rc;
}

/* ── close ────────────────────────────────────── */

long usock_close(int fd) {
    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;

    process_t *p = proc_current();
    if (p) fd_close(&p->fds, fd);

    /* usock_decref will wake peer's read_wq/write_wq for HUP propagation. */
    usock_decref(s);
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

    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != USOCK_CONNECTED || !s->peer) return send_sigpipe();

    /* Copy msghdr to kernel */
    struct k_msghdr kmsg;
    { int r = copy_from_user(&kmsg, msg_ptr, sizeof(kmsg)); if (r) return r; }

    if (kmsg.msg_iovlen == 0) return 0;
    if (kmsg.msg_iovlen > 16) return -EMSGSIZE;

    /* Copy iov array */
    struct iovec k_iov[16];
    { int r = copy_from_user(k_iov, kmsg.msg_iov, kmsg.msg_iovlen * sizeof(struct iovec)); if (r) return r; }

    /* Snapshot peer under lock */
    uint64_t irqf;
    spin_lock_irq(&usock_lock, &irqf);
    unix_socket_t *peer = s->peer;
    if (!peer) { spin_unlock_irq(&usock_lock, irqf); return send_sigpipe(); }
    spin_unlock_irq(&usock_lock, irqf);

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
            copy_from_user(kbuf, src, (size_t)chunk); /* user_ok checked per-iov above */
            spin_lock_irq(&usock_lock, &irqf);
            int w = ring_write(peer, kbuf, (int)chunk);
            spin_unlock_irq(&usock_lock, irqf);
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
        /* Wake reader(s) blocked on peer's read_wq — data just landed. */
        wake_up(&peer->read_wq);
    }
    return total;
}

long usock_recvmsg(int fd, void *msg_ptr, int flags) {
    (void)flags;

    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != USOCK_CONNECTED) return -ENOTCONN;

    /* Copy msghdr to kernel */
    struct k_msghdr kmsg;
    { int r = copy_from_user(&kmsg, msg_ptr, sizeof(kmsg)); if (r) return r; }

    if (kmsg.msg_iovlen == 0) return 0;
    if (kmsg.msg_iovlen > 16) return -EINVAL;

    /* Copy iov array */
    struct iovec k_iov[16];
    { int r = copy_from_user(k_iov, kmsg.msg_iov, kmsg.msg_iovlen * sizeof(struct iovec)); if (r) return r; }

    {
        uint64_t irqf;
        spin_lock_irq(&usock_lock, &irqf);
        if (s->count == 0) {
            int eof = !s->peer;
            spin_unlock_irq(&usock_lock, irqf);
            return eof ? 0 : -EAGAIN;
        }
        spin_unlock_irq(&usock_lock, irqf);
    }

    long total = 0;
    for (uint64_t i = 0; i < kmsg.msg_iovlen; i++) {
        if (!k_iov[i].iov_len) continue;
        if (!user_ok((uint64_t)k_iov[i].iov_base, k_iov[i].iov_len)) return -EFAULT;

        uint8_t kbuf[1024];
        uint64_t remaining = k_iov[i].iov_len;
        uint8_t *dst = (uint8_t *)k_iov[i].iov_base;
        while (remaining > 0) {
            uint64_t irqf;
            spin_lock_irq(&usock_lock, &irqf);
            if (s->count == 0) { spin_unlock_irq(&usock_lock, irqf); goto recvdone; }
            uint64_t chunk = remaining > sizeof(kbuf) ? sizeof(kbuf) : remaining;
            int r = ring_read(s, kbuf, (int)chunk);
            int cnt = s->count;
            spin_unlock_irq(&usock_lock, irqf);
            if (r <= 0) break;
            copy_to_user(dst, kbuf, (size_t)r); /* user_ok checked per-iov above */
            total += r;
            dst += r;
            remaining -= (uint64_t)r;
            if (cnt == 0) break;
        }
    }
recvdone:

    /* Zero out msg_controllen — no ancillary data */
    kmsg.msg_controllen = 0;
    kmsg.msg_flags = 0;
    copy_to_user(msg_ptr, &kmsg, sizeof(kmsg));

    if (total > 0) {
        /* Drained s->buf → peer-side writer may have room now. */
        unix_socket_t *peer = s->peer;
        if (peer) wake_up(&peer->write_wq);
    }
    return total;
}

/* sendto(AF_UNIX, flags): MSG_OOB => 1-Byte-OOB-Queue auf peer.
 * Andere Flags (MSG_DONTWAIT, MSG_NOSIGNAL) wie bei usock_write. */
#define UMSG_OOB       0x01
#define UMSG_DONTWAIT  0x40
#define UMSG_NOSIGNAL  0x4000

long usock_send(int fd, const void *buf, long len, int flags) {
    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != USOCK_CONNECTED)
        return (flags & UMSG_NOSIGNAL) ? -EPIPE : send_sigpipe();

    if (flags & UMSG_OOB) {
        if (len <= 0) return -EINVAL;
        uint8_t kbyte;
        { int r = copy_from_user(&kbyte, buf, 1); if (r) return r; }
        uint64_t irqf;
        spin_lock_irq(&usock_lock, &irqf);
        unix_socket_t *peer = s->peer;
        if (!peer) {
            spin_unlock_irq(&usock_lock, irqf);
            return (flags & UMSG_NOSIGNAL) ? -EPIPE : send_sigpipe();
        }
        peer->oob_byte = kbyte;
        peer->oob_present = 1;
        spin_unlock_irq(&usock_lock, irqf);
        wake_up(&peer->read_wq);
        return 1;
    }

    return usock_write(fd, buf, len);
}

/* recvfrom(AF_UNIX, flags): MSG_OOB => holt 1-Byte aus OOB-Queue,
 * -EINVAL wenn keine. MSG_DONTWAIT => EAGAIN statt blocken. */
long usock_recv(int fd, void *buf, long len, int flags) {
    unix_socket_t *s = usock_from_fd(fd);
    if (!s) return -EBADF;
    if (s->state != USOCK_CONNECTED) return -ENOTCONN;

    if (flags & UMSG_OOB) {
        if (len <= 0) return 0;
        uint64_t irqf;
        spin_lock_irq(&usock_lock, &irqf);
        if (!s->oob_present) {
            spin_unlock_irq(&usock_lock, irqf);
            return -EINVAL;
        }
        uint8_t kbyte = s->oob_byte;
        s->oob_present = 0;
        spin_unlock_irq(&usock_lock, irqf);
        int r = copy_to_user(buf, &kbyte, 1);
        if (r) return r;
        return 1;
    }

    if (flags & UMSG_DONTWAIT) {
        uint64_t irqf;
        spin_lock_irq(&usock_lock, &irqf);
        if (s->count == 0) {
            int eof = !s->peer;
            spin_unlock_irq(&usock_lock, irqf);
            return eof ? 0 : -EAGAIN;
        }
        spin_unlock_irq(&usock_lock, irqf);
    }

    return usock_read(fd, buf, len);
}
