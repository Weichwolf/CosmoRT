/* CosmoRT Syscall Layer — pipe, fd_cleanup, fd_poll */

#include "internal.h"

/* ── SYS_pipe2 (293) ─────────────────────────────── */

#define PIPE_BUF_SIZE 4096
#define PIPE_MAX      32

struct pipe {
    uint8_t buf[PIPE_BUF_SIZE];
    int read_pos, write_pos, count;
    int read_open, write_open;  /* refcount: >0 = open, 0 = closed */
    spinlock_t lock;
};

static struct pipe pipe_pool[PIPE_MAX];
static slab_t pipe_slab;
static int pipe_slab_inited;

static void pipe_slab_ensure(void) {
    if (!pipe_slab_inited) {
        extern void slab_init(slab_t *, void *, int, int);
        slab_init(&pipe_slab, pipe_pool, (int)sizeof(struct pipe), PIPE_MAX);
        pipe_slab_inited = 1;
    }
}

long pipe_read(struct pipe *pp, void *buf, size_t count) {
    uint64_t flags;
    spin_lock_irq(&pp->lock, &flags);
    if (pp->count == 0) {
        int wr_open = pp->write_open;
        spin_unlock_irq(&pp->lock, flags);
        return wr_open ? (long)-EAGAIN : 0; /* EOF if write end closed */
    }
    size_t n = count > (size_t)pp->count ? (size_t)pp->count : count;
    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < n; i++) {
        dst[i] = pp->buf[pp->read_pos];
        pp->read_pos = (pp->read_pos + 1) % PIPE_BUF_SIZE;
    }
    pp->count -= (int)n;
    spin_unlock_irq(&pp->lock, flags);
    return (long)n;
}

long pipe_write(struct pipe *pp, const void *buf, size_t count) {
    uint64_t flags;
    spin_lock_irq(&pp->lock, &flags);
    if (!pp->read_open) {
        spin_unlock_irq(&pp->lock, flags);
        return -EPIPE;
    }
    size_t space = (size_t)(PIPE_BUF_SIZE - pp->count);
    size_t n = count > space ? space : count;
    if (n == 0) {
        spin_unlock_irq(&pp->lock, flags);
        return (long)-EAGAIN;
    }
    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < n; i++) {
        pp->buf[pp->write_pos] = src[i];
        pp->write_pos = (pp->write_pos + 1) % PIPE_BUF_SIZE;
    }
    pp->count += (int)n;
    spin_unlock_irq(&pp->lock, flags);
    return (long)n;
}

long do_pipe2(int *fds, int flags) {
    (void)flags;
    if (!user_ok((uint64_t)fds, 2 * sizeof(int))) return -EFAULT;

    pipe_slab_ensure();
    struct pipe *pp = (struct pipe *)slab_alloc(&pipe_slab);
    if (!pp) return -ENOMEM;

    pp->read_pos = pp->write_pos = pp->count = 0;
    pp->read_open = pp->write_open = 1;
    pp->lock = (spinlock_t)SPINLOCK_INIT;

    process_t *p = proc_current();
    if (!p) { slab_free(&pipe_slab, pp); return -EFAULT; }

    int rfd = fd_alloc(&p->fds, FD_PIPE, pp, O_RDONLY);
    if (rfd < 0) { slab_free(&pipe_slab, pp); return -EMFILE; }
    int wfd = fd_alloc(&p->fds, FD_PIPE, (void *)((uint8_t *)pp + 1), O_WRONLY);
    if (wfd < 0) {
        fd_close(&p->fds, rfd);
        slab_free(&pipe_slab, pp);
        return -EMFILE;
    }
    /* Mark write-end fd: we encode read/write via pointer offset.
     * Read end: obj == pp. Write end: obj == pp+1 (non-aligned marker). */

    {
        int kfds[2] = { rfd, wfd };
        kmemcpy(fds, kfds, sizeof(kfds));
    }
    return 0;
}

/* Helper: get pipe struct + is_write from fd */
struct pipe *pipe_from_fd(fd_entry_t *fde, int *is_write) {
    if (!fde || fde->type != FD_PIPE || !fde->obj) return 0;
    /* Read end: obj is aligned to struct pipe. Write end: obj = pp + 1 byte */
    uintptr_t addr = (uintptr_t)fde->obj;
    /* Check if addr is within pipe_pool + offset 1 (write end) */
    uintptr_t base = (uintptr_t)pipe_pool;
    uintptr_t end = base + sizeof(pipe_pool);
    if (addr >= base && addr < end) {
        uintptr_t off = (addr - base) % sizeof(struct pipe);
        if (off == 0) {
            *is_write = 0;
            return (struct pipe *)addr;
        } else if (off == 1) {
            *is_write = 1;
            return (struct pipe *)(addr - 1);
        }
    }
    return 0;
}

long pipe_close(fd_entry_t *fde) {
    int is_write = 0;
    struct pipe *pp = pipe_from_fd(fde, &is_write);
    if (!pp) return -EBADF;

    uint64_t flags;
    spin_lock_irq(&pp->lock, &flags);
    if (is_write) { if (pp->write_open > 0) pp->write_open--; }
    else          { if (pp->read_open > 0)  pp->read_open--; }
    int both_closed = (pp->read_open <= 0 && pp->write_open <= 0);
    spin_unlock_irq(&pp->lock, flags);

    if (both_closed)
        slab_free(&pipe_slab, pp);
    return 0;
}

/* ── fd_cleanup_entry — process-exit cleanup for non-file FDs ── */

void fd_cleanup_entry(int fde_type, void *fde_obj) {
    if (!fde_obj) return;
    if (fde_type == FD_SOCKET) {
        socket_t *s = (socket_t *)fde_obj;
        if (s->state == SOCK_CONNECTED)
            net_tcp_close(&s->tcp);
        s->state = SOCK_UNUSED;
    } else if (fde_type == FD_PIPE) {
        fd_entry_t tmp = { FD_PIPE, fde_obj, 0 };
        pipe_close(&tmp);
    } else if (fde_type == FD_EPOLL) {
        epoll_destroy(fde_obj);
    } else if (fde_type == FD_EVENTFD) {
        eventfd_destroy(fde_obj);
    } else if (fde_type == FD_TIMERFD) {
        timerfd_destroy(fde_obj);
    } else if (fde_type == FD_INOTIFY) {
        inotify_destroy(fde_obj);
    }
}

/* ── fd_obj_incref — bump refcount for fork/dup of non-file FDs ── */

void fd_obj_incref(int fde_type, void *fde_obj) {
    if (!fde_obj) return;
    if (fde_type == FD_PIPE) {
        int is_write = 0;
        fd_entry_t tmp = { FD_PIPE, fde_obj, 0 };
        struct pipe *pp = pipe_from_fd(&tmp, &is_write);
        if (pp) {
            uint64_t flags;
            spin_lock_irq(&pp->lock, &flags);
            if (is_write) pp->write_open++;
            else          pp->read_open++;
            spin_unlock_irq(&pp->lock, flags);
        }
    }
    /* TODO: refcounting for sockets, epoll, eventfd, etc. */
}

/* ── fd_poll_readiness — check what events are ready on an FD ── */

uint32_t fd_poll_readiness(int fd, uint32_t interest) {
    process_t *p = proc_current();
    if (!p) return EPOLLHUP | EPOLLERR;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type == FD_NONE) return EPOLLHUP | EPOLLERR;

    uint32_t ready = 0;

    switch (fde->type) {
    case FD_SERIAL:
        if (interest & EPOLLOUT) ready |= EPOLLOUT;
        break;

    case FD_SOCKET: {
        socket_t *s = (socket_t *)fde->obj;
        if (!s) { ready |= EPOLLERR; break; }
        if ((interest & EPOLLIN) && s->state == SOCK_CONNECTED) {
            extern pkt_queue_t q_tcp;
            if (s->tcp.rxbuf_pos < s->tcp.rxbuf_len || q_tcp.count > 0)
                ready |= EPOLLIN;
        }
        if ((interest & EPOLLOUT) && s->state == SOCK_CONNECTED)
            ready |= EPOLLOUT;
        break;
    }

    case FD_PIPE: {
        int is_write = 0;
        struct pipe *pp = pipe_from_fd(fde, &is_write);
        if (!pp) { ready |= EPOLLERR; break; }
        if (!is_write) {
            if (interest & EPOLLIN) {
                if (pp->count > 0) ready |= EPOLLIN;
                if (!pp->write_open) ready |= EPOLLIN | EPOLLHUP;
            }
        } else {
            if (interest & EPOLLOUT) {
                if (pp->count < PIPE_BUF_SIZE) ready |= EPOLLOUT;
                if (!pp->read_open) ready |= EPOLLERR | EPOLLHUP;
            }
        }
        break;
    }

    case FD_EVENTFD: {
        eventfd_t *efd = (eventfd_t *)fde->obj;
        if (!efd) { ready |= EPOLLERR; break; }
        if ((interest & EPOLLIN) && efd->counter > 0) ready |= EPOLLIN;
        if (interest & EPOLLOUT) ready |= EPOLLOUT;
        break;
    }

    case FD_TIMERFD: {
        timerfd_t *tfd = (timerfd_t *)fde->obj;
        if (!tfd) { ready |= EPOLLERR; break; }
        /* Check for expired timer */
        if (tfd->armed && timer_ms() >= tfd->expire_ms) {
            uint64_t irqf;
            spin_lock_irq(&tfd->lock, &irqf);
            while (tfd->armed && timer_ms() >= tfd->expire_ms) {
                tfd->expirations++;
                if (tfd->interval_ms > 0)
                    tfd->expire_ms += tfd->interval_ms;
                else
                    tfd->armed = 0;
            }
            spin_unlock_irq(&tfd->lock, irqf);
        }
        if ((interest & EPOLLIN) && tfd->expirations > 0) ready |= EPOLLIN;
        break;
    }

    case FD_FILE:
        if (interest & EPOLLIN)  ready |= EPOLLIN;
        if (interest & EPOLLOUT) ready |= EPOLLOUT;
        break;

    case FD_INOTIFY:
        if ((interest & EPOLLIN) && inotify_has_events(fde->obj))
            ready |= EPOLLIN;
        break;

    default:
        if (interest & EPOLLOUT) ready |= EPOLLOUT;
        break;
    }

    return ready;
}
