/* CosmoRT Syscall Layer — pipe, fd_cleanup, fd_poll */

#include "internal.h"
#include "vt/pty.h"
#include "core/event_queue.h"
#include "linux/capability.h"

/* ── SYS_pipe2 (293) ─────────────────────────────── */

#define PIPE_BUF_DEFAULT  65536     /* Linux PIPE_DEF_BUFFERS*PAGE_SIZE = 16*4K */
#define PIPE_BUF_MAX      1048576   /* /proc/sys/fs/pipe-max-size default */
#define PIPE_BUF_MIN_PAGE 4096      /* kleinste zulaessige pipe-max-size */

/* Dynamische Grenze aus /proc/sys/fs/pipe-max-size. Unprivileged pipe_alloc
 * und F_SETPIPE_SZ respektieren diesen Wert; root (euid==0) darf drueber. */
static int g_pipe_max_size = PIPE_BUF_MAX;

int pipe_max_size_get(void) { return g_pipe_max_size; }

long pipe_max_size_set(int v) {
    if (v < PIPE_BUF_MIN_PAGE) return -EINVAL;
    if ((unsigned int)v >= (1U << 31)) return -EINVAL;
    int pages = (v + 4095) / 4096;
    g_pipe_max_size = pages * 4096;
    return 0;
}

/* Async-signal owner per pipe-end. Linux F_SETOWN/F_SETSIG sind per
 * struct file — wir haben nur einen struct pipe fuer beide Enden,
 * speichern deshalb getrennte Owner fuer Lese/Schreib-Seite. */
#define PIPE_OWNER_PID  0
#define PIPE_OWNER_PGRP 1
#define PIPE_OWNER_TID  2
struct pipe_owner {
    int pid;        /* F_SETOWN: >0 pid, <0 pgid. F_SETOWN_EX: tid oder pid je nach type */
    int sig;        /* Signal-Nummer (0 = default SIGIO) */
    int o_async;    /* O_ASYNC aktiv (F_SETFL) — ohne das kein Signal */
    int type;       /* PIPE_OWNER_{PID,PGRP,TID} (nur fuer F_SETOWN_EX relevant) */
};

struct pipe {
    uint8_t *buf;               /* dynamically allocated, size == buf_size (page-multiple) */
    int buf_size;               /* current ring capacity in bytes */
    int read_pos, write_pos, count;
    int read_open, write_open;  /* refcount: >0 = open, 0 = closed */
    int was_full;               /* Linux-Ring-Semantik: EPOLLOUT-Edge nur nach komplettem Drain */
    thread_t *blocked_reader;   /* thread blocked in pipe read */
    thread_t *blocked_writer;   /* thread blocked in pipe write */
    struct pipe_owner owner[2]; /* [0] = reader-end, [1] = writer-end */
    spinlock_t lock;
};

static slab_t pipe_slab;
static int pipe_slab_inited;

static void pipe_slab_ensure(void) {
    if (__sync_bool_compare_and_swap(&pipe_slab_inited, 0, 1))
        slab_init_dynamic(&pipe_slab, (int)sizeof(struct pipe), 0);
}

/* Round bytes up to whole pages (4KiB) and return the page count.
 * Linux pipe_resize_ring rounds requested size up to page-granularity. */
static int pipe_page_count(int bytes) {
    if (bytes <= 0) return 1;
    return (bytes + 4095) / 4096;
}

int pipe_get_size(struct pipe *pp) {
    return pp ? pp->buf_size : 0;
}

/* F_SETPIPE_SZ — resize ring. Caller passes requested byte size.
 * Linux semantics (fs/pipe.c:pipe_set_size → pipe_resize_ring):
 *   - Round up to page size
 *   - EBUSY if new_size < current fill
 *   - Copy existing data into new buffer
 *   - Return new size on success
 * Returns new size in bytes, or -errno. */
long pipe_resize(struct pipe *pp, int new_size) {
    if (!pp) return -EINVAL;
    int pages = pipe_page_count(new_size);
    int rounded = pages * 4096;

    uint64_t flags;
    spin_lock_irq(&pp->lock, &flags);
    if (rounded < pp->count) {
        spin_unlock_irq(&pp->lock, flags);
        return -EBUSY;
    }
    if (rounded == pp->buf_size) {
        spin_unlock_irq(&pp->lock, flags);
        return rounded;
    }
    spin_unlock_irq(&pp->lock, flags);

    /* Allocate without holding lock (may sleep in buddy) */
    uint8_t *nbuf = (uint8_t *)pages_alloc(pages);
    if (!nbuf) return -ENOMEM;

    spin_lock_irq(&pp->lock, &flags);
    /* Re-check EBUSY after reacquiring — another writer may have filled */
    if (rounded < pp->count) {
        spin_unlock_irq(&pp->lock, flags);
        pages_free(nbuf, pages);
        return -EBUSY;
    }
    uint8_t *old = pp->buf;
    int old_pages = pipe_page_count(pp->buf_size);
    for (int i = 0; i < pp->count; i++) {
        nbuf[i] = pp->buf[(pp->read_pos + i) % pp->buf_size];
    }
    pp->buf = nbuf;
    pp->buf_size = rounded;
    pp->read_pos = 0;
    pp->write_pos = pp->count;
    if (pp->count < rounded) pp->was_full = 0;
    spin_unlock_irq(&pp->lock, flags);

    pages_free(old, old_pages);
    return rounded;
}

/* Linux SIGIO default fuer Async-FD-Events. fcntl F_SETSIG kann das
 * ueberschreiben (0 = Default SIGIO). */
#define PIPE_SIGIO_DEFAULT  29   /* SIGIO */

static void pipe_notify_end(struct pipe_owner *o) {
    if (!o->o_async || o->pid == 0) return;
    int sig = o->sig ? o->sig : PIPE_SIGIO_DEFAULT;
    if (o->type == PIPE_OWNER_TID) {
        /* F_OWNER_TID: target a specific thread. do_tgkill(tgid, tid, sig).
         * We dont track tgid per pipe-owner — lookup via tid -> thread -> proc.
         * Fallback to do_kill(0, sig) would lose the TID-precision; Linux uses
         * __send_sig_info mit t->group_leader. */
        extern long do_tgkill(int tgid, int tid, int sig);
        extern thread_t *thread_find_by_tid(int tid);
        thread_t *t = thread_find_by_tid(o->pid);
        if (t && t->proc) do_tgkill((int)t->proc->pid, o->pid, sig);
        return;
    }
    /* F_OWNER_PID / F_OWNER_PGRP / legacy F_SETOWN: pid>0 process, pid<0 pgid */
    do_kill(o->pid, sig);
}

/* fcntl F_SETOWN/F_GETOWN/F_SETSIG/F_GETSIG Zugriff ueber fd-end-Index */
long pipe_fcntl_getown(struct pipe *pp, int end) {
    uint64_t flags;
    spin_lock_irq(&pp->lock, &flags);
    int v = pp->owner[end].pid;
    spin_unlock_irq(&pp->lock, flags);
    return v;
}

long pipe_fcntl_setown(struct pipe *pp, int end, int arg) {
    uint64_t flags;
    spin_lock_irq(&pp->lock, &flags);
    pp->owner[end].pid = arg;
    pp->owner[end].type = (arg < 0) ? PIPE_OWNER_PGRP : PIPE_OWNER_PID;
    spin_unlock_irq(&pp->lock, flags);
    return 0;
}

long pipe_fcntl_setown_ex(struct pipe *pp, int end, int who, int type) {
    uint64_t flags;
    spin_lock_irq(&pp->lock, &flags);
    pp->owner[end].pid = who;
    pp->owner[end].type = type;
    spin_unlock_irq(&pp->lock, flags);
    return 0;
}

long pipe_fcntl_getsig(struct pipe *pp, int end) {
    uint64_t flags;
    spin_lock_irq(&pp->lock, &flags);
    int v = pp->owner[end].sig;
    spin_unlock_irq(&pp->lock, flags);
    return v;
}

long pipe_fcntl_setsig(struct pipe *pp, int end, int sig) {
    if (sig < 0 || sig >= 64) return -EINVAL;
    uint64_t flags;
    spin_lock_irq(&pp->lock, &flags);
    pp->owner[end].sig = sig;
    spin_unlock_irq(&pp->lock, flags);
    return 0;
}

void pipe_set_async(struct pipe *pp, int end, int on) {
    uint64_t flags;
    spin_lock_irq(&pp->lock, &flags);
    pp->owner[end].o_async = on ? 1 : 0;
    spin_unlock_irq(&pp->lock, flags);
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
        pp->read_pos = (pp->read_pos + 1) % pp->buf_size;
    }
    pp->count -= (int)n;
    if (pp->count == 0) pp->was_full = 0;
    /* Wake blocked writer if space freed */
    thread_t *writer = pp->blocked_writer;
    pp->blocked_writer = 0;
    /* Snapshot writer-end owner fuer SIGIO nach Lock-Release */
    struct pipe_owner notify_w = pp->owner[1];
    spin_unlock_irq(&pp->lock, flags);
    if (writer) {
        extern void event_post(thread_t *target, uint32_t type, uint64_t data);
        event_post(writer, 4 /* EQ_PIPE_DATA */, (uint64_t)n);
    }
    pipe_notify_end(&notify_w);
    /* Wake epoll/poll sleepers — pipe now writable */
    extern void epoll_wake_all(void);
    epoll_wake_all();
    return (long)n;
}

long pipe_write(struct pipe *pp, const void *buf, size_t count) {
    uint64_t flags;
    spin_lock_irq(&pp->lock, &flags);
    if (!pp->read_open) {
        spin_unlock_irq(&pp->lock, flags);
        return send_sigpipe();
    }
    size_t space = (size_t)(pp->buf_size - pp->count);
    size_t n = count > space ? space : count;
    if (n == 0) {
        spin_unlock_irq(&pp->lock, flags);
        return (long)-EAGAIN;
    }
    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < n; i++) {
        pp->buf[pp->write_pos] = src[i];
        pp->write_pos = (pp->write_pos + 1) % pp->buf_size;
    }
    pp->count += (int)n;
    if (pp->count >= pp->buf_size) pp->was_full = 1;
    /* Wake blocked reader */
    thread_t *reader = pp->blocked_reader;
    pp->blocked_reader = 0;
    /* Snapshot reader-end owner fuer SIGIO nach Lock-Release */
    struct pipe_owner notify_r = pp->owner[0];
    spin_unlock_irq(&pp->lock, flags);
    if (reader) {
        extern void event_post(thread_t *target, uint32_t type, uint64_t data);
        event_post(reader, 4 /* EQ_PIPE_DATA */, (uint64_t)n);
    }
    pipe_notify_end(&notify_r);
    /* Wake epoll/poll sleepers — pipe now readable */
    extern void epoll_wake_all(void);
    epoll_wake_all();
    return (long)n;
}

/* Blocking pipe read: called when pipe_read returned -EAGAIN.
 * Re-checks under lock, blocks if still empty, restarts syscall on wake. */
long pipe_read_blocking(struct pipe *pp, void *buf, size_t count) {
    extern void event_post(thread_t *target, uint32_t type, uint64_t data);
    thread_t *t = thread_current();
    if (!t) return -EAGAIN;

    for (;;) {
        uint64_t irqf;
        spin_lock_irq(&pp->lock, &irqf);
        /* Re-check under lock — data may have arrived */
        if (pp->count > 0) {
            size_t n = count > (size_t)pp->count ? (size_t)pp->count : count;
            uint8_t *dst = (uint8_t *)buf;
            for (size_t i = 0; i < n; i++) {
                dst[i] = pp->buf[pp->read_pos];
                pp->read_pos = (pp->read_pos + 1) % pp->buf_size;
            }
            pp->count -= (int)n;
            if (pp->count == 0) pp->was_full = 0;
            /* Wake blocked writer */
            thread_t *writer = pp->blocked_writer;
            pp->blocked_writer = 0;
            struct pipe_owner notify_w = pp->owner[1];
            spin_unlock_irq(&pp->lock, irqf);
            if (writer)
                event_post(writer, 4 /* EQ_PIPE_DATA */, (uint64_t)n);
            pipe_notify_end(&notify_w);
            return (long)n;
        }
        if (!pp->write_open) {
            spin_unlock_irq(&pp->lock, irqf);
            return 0; /* EOF */
        }
        pp->blocked_reader = t;
        spin_unlock_irq(&pp->lock, irqf);
        /* Check pending signals before blocking (POSIX -EINTR semantics) */
        if (t->proc) {
            uint64_t deliverable = t->proc->sig_pending & ~t->sig_blocked;
            if (deliverable) return -EINTR;
        }
        /* Block via event_wait — pipe_write/pipe_close will event_post us.
         * If event pre-queued, returns immediately → loop retries. */
        event_t ev;
        int _wr = event_wait(&t->eq, &ev, -1);
        if (_wr == -4) return -EINTR;
    }
}

/* Blocking pipe write: called when pipe_write returned -EAGAIN (full).
 * Re-checks under lock, blocks if still full, restarts syscall on wake. */
long pipe_write_blocking(struct pipe *pp, const void *buf, size_t count) {
    extern void event_post(thread_t *target, uint32_t type, uint64_t data);
    thread_t *t = thread_current();
    if (!t) return -EAGAIN;

    for (;;) {
        uint64_t irqf;
        spin_lock_irq(&pp->lock, &irqf);
        /* Re-check under lock */
        if (pp->count < pp->buf_size) {
            size_t space = (size_t)(pp->buf_size - pp->count);
            size_t n = count > space ? space : count;
            const uint8_t *src = (const uint8_t *)buf;
            for (size_t i = 0; i < n; i++) {
                pp->buf[pp->write_pos] = src[i];
                pp->write_pos = (pp->write_pos + 1) % pp->buf_size;
            }
            pp->count += (int)n;
            if (pp->count >= pp->buf_size) pp->was_full = 1;
            /* Wake blocked reader */
            thread_t *reader = pp->blocked_reader;
            pp->blocked_reader = 0;
            struct pipe_owner notify_r = pp->owner[0];
            spin_unlock_irq(&pp->lock, irqf);
            if (reader)
                event_post(reader, 4 /* EQ_PIPE_DATA */, (uint64_t)n);
            pipe_notify_end(&notify_r);
            return (long)n;
        }
        if (!pp->read_open) {
            spin_unlock_irq(&pp->lock, irqf);
            return send_sigpipe();
        }
        pp->blocked_writer = t;
        spin_unlock_irq(&pp->lock, irqf);
        /* Check pending signals before blocking (POSIX -EINTR semantics) */
        if (t->proc) {
            uint64_t deliverable = t->proc->sig_pending & ~t->sig_blocked;
            if (deliverable) return -EINTR;
        }
        /* Block via event_wait — pipe_read/pipe_close will event_post us */
        event_t ev;
        int _wr = event_wait(&t->eq, &ev, -1);
        if (_wr == -4) return -EINTR;
    }
}

long do_pipe2(int *fds, int flags) {
    if (!user_ok((uint64_t)fds, 2 * sizeof(int))) return -EFAULT; /* validated early, copy_to_user below */

    pipe_slab_ensure();
    struct pipe *pp = (struct pipe *)slab_alloc(&pipe_slab);
    if (!pp) return -ENOMEM;
    /* Linux commit 086e774a57fb: unprivileged pipe() capped to pipe-max-size.
     * CAP_SYS_RESOURCE darf Default behalten, selbst wenn pipe-max-size
     * kleiner ist. (LTP fcntl35 droppt die Cap, also nicht ueber euid). */
    int init_size = PIPE_BUF_DEFAULT;
    process_t *cur = proc_current();
    int has_cap = cur && (cur->cap_effective & CAP_TO_MASK(CAP_SYS_RESOURCE));
    if (cur && !has_cap && g_pipe_max_size < init_size)
        init_size = g_pipe_max_size;
    int init_pages = pipe_page_count(init_size);
    pp->buf = (uint8_t *)pages_alloc(init_pages);
    if (!pp->buf) { slab_free(&pipe_slab, pp); return -ENOMEM; }
    pp->buf_size = init_pages * 4096;

    pp->read_pos = pp->write_pos = pp->count = 0;
    pp->read_open = pp->write_open = 1;
    pp->was_full = 0;
    pp->blocked_reader = 0;
    pp->blocked_writer = 0;
    pp->owner[0].pid = pp->owner[0].sig = pp->owner[0].o_async = pp->owner[0].type = 0;
    pp->owner[1].pid = pp->owner[1].sig = pp->owner[1].o_async = pp->owner[1].type = 0;
    pp->lock = (spinlock_t)SPINLOCK_INIT;

    process_t *p = cur;
    if (!p) { pages_free(pp->buf, init_pages); slab_free(&pipe_slab, pp); return -EFAULT; }

    /* Build fd flags: carry over O_CLOEXEC and O_NONBLOCK from pipe2 flags */
    int rflags = O_RDONLY;
    int wflags = O_WRONLY;
    if (flags & O_CLOEXEC)  { rflags |= O_CLOEXEC;  wflags |= O_CLOEXEC; }
    if (flags & O_NONBLOCK) { rflags |= O_NONBLOCK;  wflags |= O_NONBLOCK; }

    int rfd = fd_alloc(&p->fds, FD_PIPE, pp, rflags);
    if (rfd < 0) {
        pages_free(pp->buf, pipe_page_count(pp->buf_size));
        slab_free(&pipe_slab, pp);
        return -EMFILE;
    }
    int wfd = fd_alloc(&p->fds, FD_PIPE, pp, wflags);
    if (wfd < 0) {
        fd_close(&p->fds, rfd);
        pages_free(pp->buf, pipe_page_count(pp->buf_size));
        slab_free(&pipe_slab, pp);
        return -EMFILE;
    }
    /* Read vs. write end encoded in fde->flags (O_RDONLY/O_WRONLY). */

    {
        /* Linux ABI: int[2]. But ktest uses long[2] via raw syscall wrappers.
         * Write int[2] — matches Linux pipe(2) ABI exactly. */
        int kfds[2] = { rfd, wfd };
        copy_to_user(fds, kfds, sizeof(kfds));
    }
    return 0;
}

/* Helper: get pipe struct + is_write from fd.
 * Read/write end distinguished via fde->flags (O_RDONLY=0, O_WRONLY=1). */
struct pipe *pipe_from_fd(fd_entry_t *fde, int *is_write) {
    if (!fde || fde->type != FD_PIPE || !fde->obj) return 0;
    *is_write = (fde->flags & O_WRONLY) ? 1 : 0;
    return (struct pipe *)fde->obj;
}

long pipe_close(fd_entry_t *fde) {
    int is_write = 0;
    struct pipe *pp = pipe_from_fd(fde, &is_write);
    if (!pp) return -EBADF;

    uint64_t flags;
    spin_lock_irq(&pp->lock, &flags);
    if (is_write) { if (pp->write_open > 0) pp->write_open--; }
    else          {
        if (pp->read_open > 0) pp->read_open--;
    }
    int both_closed = (pp->read_open <= 0 && pp->write_open <= 0);
    /* Wake blocked reader if write end closed (EOF) */
    thread_t *reader = 0;
    if (is_write && pp->write_open <= 0 && pp->blocked_reader) {
        reader = pp->blocked_reader;
        pp->blocked_reader = 0;
    }
    /* Wake blocked writer if read end closed (EPIPE) */
    thread_t *writer = 0;
    if (!is_write && pp->read_open <= 0 && pp->blocked_writer) {
        writer = pp->blocked_writer;
        pp->blocked_writer = 0;
    }
    spin_unlock_irq(&pp->lock, flags);

    if (reader) {
        extern void event_post(thread_t *target, uint32_t type, uint64_t data);
        event_post(reader, 5 /* EQ_PIPE_CLOSED */, 0);
    }
    if (writer) {
        extern void event_post(thread_t *target, uint32_t type, uint64_t data);
        event_post(writer, 5 /* EQ_PIPE_CLOSED */, 0);
    }
    if (both_closed) {
        pages_free(pp->buf, pipe_page_count(pp->buf_size));
        slab_free(&pipe_slab, pp);
    }
    /* Wake epoll/poll sleepers — pipe state changed (HUP/ERR) */
    extern void epoll_wake_all(void);
    epoll_wake_all();
    return 0;
}

/* ── fd_cleanup_entry — process-exit cleanup for non-file FDs ── */

void fd_cleanup_entry(int fde_type, void *fde_obj, int fde_flags) {
    if (!fde_obj) return;
    if (fde_type == FD_SOCKET) {
        socket_t *s = (socket_t *)fde_obj;
        if (__sync_sub_and_fetch(&s->refcount, 1) <= 0) {
            if (s->is_dgram && s->udp_local_port) {
                udp_sock_t *us = udp_find(s->udp_local_port);
                if (us) udp_unbind(us);
            }
            if (!s->is_dgram) {
                if (s->state == SOCK_CONNECTED)
                    net_tcp_close(&s->tcp);
                else if (s->tcp.state != TCP_CLOSED) {
                    /* Deregister non-connected but registered TCP connections
                     * (e.g. SYN_SENT from failed connect). Prevents dangling hash entry. */
                    extern void tcp_unregister(net_tcp_t *c);
                    tcp_unregister(&s->tcp);
                    s->tcp.state = TCP_CLOSED;
                }
            }
            sock_free(s);
        }
    } else if (fde_type == FD_PIPE) {
        fd_entry_t tmp = { FD_PIPE, fde_obj, fde_flags };
        pipe_close(&tmp);
    } else if (fde_type == FD_EPOLL) {
        epoll_destroy(fde_obj);
    } else if (fde_type == FD_EVENTFD) {
        eventfd_destroy(fde_obj);
    } else if (fde_type == FD_TIMERFD) {
        timerfd_destroy(fde_obj);
    } else if (fde_type == FD_INOTIFY) {
        inotify_destroy(fde_obj);
    } else if (fde_type == FD_UNIX_SOCK) {
        usock_decref(fde_obj);
    }
}

/* ── fd_obj_incref — bump refcount for fork/dup of non-file FDs ── */

void fd_obj_incref(int fde_type, void *fde_obj, int fde_flags) {
    if (!fde_obj) return;
    if (fde_type == FD_PIPE) {
        struct pipe *pp = (struct pipe *)fde_obj;
        int is_write = (fde_flags & O_WRONLY) ? 1 : 0;
        uint64_t flags;
        spin_lock_irq(&pp->lock, &flags);
        if (is_write) pp->write_open++;
        else          pp->read_open++;
        spin_unlock_irq(&pp->lock, flags);
    }
    if (fde_type == FD_SOCKET) {
        socket_t *s = (socket_t *)fde_obj;
        __sync_add_and_fetch(&s->refcount, 1);
    }
    if (fde_type == FD_UNIX_SOCK) {
        usock_incref(fde_obj);
    }
    if (fde_type == FD_EPOLL) {
        extern void epoll_incref(void *obj);
        epoll_incref(fde_obj);
    }
    if (fde_type == FD_EVENTFD) {
        eventfd_t *efd = (eventfd_t *)fde_obj;
        __sync_add_and_fetch(&efd->refcount, 1);
    }
    if (fde_type == FD_TIMERFD) {
        timerfd_t *tfd = (timerfd_t *)fde_obj;
        __sync_add_and_fetch(&tfd->refcount, 1);
    }
    if (fde_type == FD_INOTIFY) {
        inotify_incref(fde_obj);
    }
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
        /* Serial input available? Check LSR bit 0 */
        if (interest & EPOLLIN) {
            extern int serial_data_available(void);
            if (serial_data_available()) ready |= EPOLLIN;
        }
        break;

    case FD_PTY_SLAVE:
    case FD_PTY_MASTER: {
        int pty_id = (int)(long)fde->obj;
        extern pty_t *pty_get(int id);
        pty_t *pt = pty_get(pty_id);
        if (!pt) { ready |= EPOLLERR; break; }
        if (interest & EPOLLIN) {
            int avail = (pt->input_tail - pt->input_head + PTY_BUF_SIZE) % PTY_BUF_SIZE;
            if (avail > 0) ready |= EPOLLIN;
        }
        if (interest & EPOLLOUT) ready |= EPOLLOUT; /* always writable */
        break;
    }

    case FD_SOCKET: {
        socket_t *s = (socket_t *)fde->obj;
        if (!s) { ready |= EPOLLERR; break; }
        if (s->is_dgram) {
            /* UDP: check if packets available in per-socket UDP queue */
            if (interest & EPOLLIN) {
                if (udp_poll_ready(s->udp_local_port))
                    ready |= EPOLLIN;
            }
            if (interest & EPOLLOUT)
                ready |= EPOLLOUT; /* UDP always writable */
        } else {
            /* TCP */
            if ((interest & EPOLLIN) && s->state == SOCK_CONNECTED) {
                extern pkt_queue_t q_tcp;
                if (rxring_used(&s->tcp.rx) > 0 || q_tcp.count > 0)
                    ready |= EPOLLIN;
            }
            /* Listener readable: pending SYN in q_tcp for our port (Linux:
             * select/poll wakes on incoming connection before accept()). */
            if ((interest & EPOLLIN) && s->state == SOCK_LISTENING) {
                extern pkt_queue_t q_tcp;
                if (q_tcp.count > 0) ready |= EPOLLIN;
            }
            if (interest & EPOLLOUT) {
                if (s->state == SOCK_CONNECTED)
                    ready |= EPOLLOUT;
                /* Non-blocking connect completed */
                if ((s->sockflags & SOCKF_CONNECTING) &&
                    s->tcp.state == TCP_ESTABLISHED)
                    ready |= EPOLLOUT;
            }
            /* Non-blocking connect failed */
            if ((s->sockflags & SOCKF_CONNECTING) && s->tcp.got_rst)
                ready |= EPOLLERR;
            /* EPOLLRDHUP: reading half closed (SHUT_RD oder Peer-FIN).
             * Wird in epoll.c immer gemeldet, unabhaengig von interest-Mask,
             * analog Linux-Verhalten fuer EPOLLHUP/EPOLLERR. */
            if (s->shut_rd || s->tcp.state == TCP_CLOSE_WAIT ||
                s->tcp.state == TCP_CLOSED)
                ready |= EPOLLRDHUP;
        }
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
            if (!pp->write_open) ready |= EPOLLRDHUP;
        } else {
            if (interest & EPOLLOUT) {
                /* Linux-Ring-Semantik: Einmal voll, erst nach komplettem Drain
                 * wieder writable. Matcht pipe_release_buf (fs/pipe.c). */
                if (pp->count < pp->buf_size && !pp->was_full)
                    ready |= EPOLLOUT;
                if (!pp->read_open) ready |= EPOLLERR | EPOLLHUP;
            }
        }
        break;
    }

    case FD_EVENTFD: {
        eventfd_t *efd = (eventfd_t *)fde->obj;
        if (!efd) { ready |= EPOLLERR; break; }
        if ((interest & EPOLLIN) && efd->counter > 0) ready |= EPOLLIN;
        /* Linux fs/eventfd.c: writable wenn counter < EVENTFD_ULLONG_MAX-1
         * (noch Platz fuer +1); sonst kein EPOLLOUT. Matcht eventfd04. */
        if ((interest & EPOLLOUT) && efd->counter < 0xFFFFFFFFFFFFFFFEULL)
            ready |= EPOLLOUT;
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

    case FD_UNIX_SOCK: {
        unix_socket_t *us = (unix_socket_t *)fde->obj;
        if (!us) { ready |= EPOLLERR; break; }
        if (interest & EPOLLIN) {
            if (us->count > 0) ready |= EPOLLIN;
            if (!us->peer) ready |= EPOLLIN | EPOLLHUP; /* EOF */
        }
        if (interest & EPOLLOUT) {
            if (!us->peer) ready |= EPOLLERR | EPOLLHUP;
            else if (us->peer->count < USOCK_BUF_SIZE) ready |= EPOLLOUT;
        }
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
