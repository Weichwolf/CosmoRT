/* CosmoRT epoll/eventfd/timerfd/signalfd/inotify
 *
 * epoll: core event multiplexing (libuv/Node.js)
 * eventfd: lightweight inter-thread signalling
 * timerfd: timer as file descriptor
 * signalfd/inotify: stubs (programs fall back to polling)
 *
 * Blocking: hlt-loop with IRQ wakeup, same pattern as do_poll in socket.c.
 */

#include "epoll.h"
#include "syscall.h"
#include "process.h"
#include "percpu.h"
#include "serial.h"
#include "slab.h"
#include "spinlock.h"
#include "timer.h"
#include "fd.h"
#include "memops.h"
#include "net.h"

/* ── Validate user pointer ───────────────────────── */

static inline int user_ok(uint64_t addr, size_t len) {
    return addr < 0x800000000000ULL &&
           addr + len <= 0x800000000000ULL &&
           addr + len >= addr;
}

/* ── Epoll internals ─────────────────────────────── */

#define EPOLL_MAX_FDS   64
#define EPOLL_POOL_MAX  16

typedef struct {
    int      fd;
    uint32_t events;
    uint64_t data;
} epoll_entry_t;

typedef struct {
    epoll_entry_t entries[EPOLL_MAX_FDS];
    int           count;
    spinlock_t    lock;
} epoll_t;

static epoll_t epoll_pool[EPOLL_POOL_MAX];
static slab_t  epoll_slab;

/* ── Eventfd pool ────────────────────────────────── */

#define EVENTFD_POOL_MAX 32

static eventfd_t eventfd_pool[EVENTFD_POOL_MAX];
static slab_t    eventfd_slab;

/* ── Timerfd pool ────────────────────────────────── */

#define TIMERFD_POOL_MAX 16

static timerfd_t timerfd_pool[TIMERFD_POOL_MAX];
static slab_t    timerfd_slab;

/* ── Inotify internals ───────────────────────────── */

#define INOTIFY_POOL_MAX   8
#define INOTIFY_WATCH_MAX  8
#define INOTIFY_EVENT_MAX  32

typedef struct {
    int      wd;
    uint32_t mask;
    char     name[256];
} inotify_queued_event_t;

typedef struct {
    int      wd;
    uint32_t mask;
    char     path[256];    /* watched directory path */
    int      active;
} inotify_watch_t;

typedef struct {
    inotify_watch_t        watches[INOTIFY_WATCH_MAX];
    int                    watch_count;
    int                    next_wd;
    inotify_queued_event_t events[INOTIFY_EVENT_MAX];
    int                    event_head;
    int                    event_tail;
    spinlock_t             lock;
} inotify_t;

static inotify_t inotify_pool[INOTIFY_POOL_MAX];
static slab_t    inotify_slab;

/* ── Init ────────────────────────────────────────── */

void epoll_init(void) {
    slab_init(&epoll_slab, epoll_pool, (int)sizeof(epoll_t), EPOLL_POOL_MAX);
    slab_init(&eventfd_slab, eventfd_pool, (int)sizeof(eventfd_t), EVENTFD_POOL_MAX);
    slab_init(&timerfd_slab, timerfd_pool, (int)sizeof(timerfd_t), TIMERFD_POOL_MAX);
    slab_init(&inotify_slab, inotify_pool, (int)sizeof(inotify_t), INOTIFY_POOL_MAX);
    serial_puts("epoll: init\n");
}

/* ── SYS_EPOLL_CREATE1 (291) ────────────────────── */

long do_epoll_create1(int flags) {
    (void)flags;
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    epoll_t *ep = (epoll_t *)slab_alloc(&epoll_slab);
    if (!ep) return -ENOMEM;

    ep->count = 0;
    ep->lock = (spinlock_t)SPINLOCK_INIT;

    int fd = fd_alloc(&p->fds, FD_EPOLL, ep, O_RDWR);
    if (fd < 0) {
        slab_free(&epoll_slab, ep);
        return -EMFILE;
    }
    return fd;
}

/* ── SYS_EPOLL_CTL (233) ────────────────────────── */

long do_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *epfde = fd_get(&p->fds, epfd);
    if (!epfde || epfde->type != FD_EPOLL) return -EBADF;
    epoll_t *ep = (epoll_t *)epfde->obj;
    if (!ep) return -EBADF;

    /* Validate target fd exists (except for DEL, target may already be closed) */
    if (op != EPOLL_CTL_DEL) {
        fd_entry_t *tgt = fd_get(&p->fds, fd);
        if (!tgt || tgt->type == FD_NONE) return -EBADF;
    }

    uint64_t irqf;
    spin_lock_irq(&ep->lock, &irqf);

    switch (op) {
    case EPOLL_CTL_ADD: {
        if (!event || !user_ok((uint64_t)event, sizeof(*event))) {
            spin_unlock_irq(&ep->lock, irqf);
            return -EFAULT;
        }
        for (int i = 0; i < ep->count; i++) {
            if (ep->entries[i].fd == fd) {
                spin_unlock_irq(&ep->lock, irqf);
                return -EEXIST;
            }
        }
        if (ep->count >= EPOLL_MAX_FDS) {
            spin_unlock_irq(&ep->lock, irqf);
            return -ENOMEM;
        }
        struct epoll_event kev;
        kmemcpy(&kev, event, sizeof(kev));
        ep->entries[ep->count].fd     = fd;
        ep->entries[ep->count].events = kev.events;
        ep->entries[ep->count].data   = kev.data;
        ep->count++;
        break;
    }
    case EPOLL_CTL_MOD: {
        if (!event || !user_ok((uint64_t)event, sizeof(*event))) {
            spin_unlock_irq(&ep->lock, irqf);
            return -EFAULT;
        }
        struct epoll_event kev;
        kmemcpy(&kev, event, sizeof(kev));
        int found = 0;
        for (int i = 0; i < ep->count; i++) {
            if (ep->entries[i].fd == fd) {
                ep->entries[i].events = kev.events;
                ep->entries[i].data   = kev.data;
                found = 1;
                break;
            }
        }
        if (!found) {
            spin_unlock_irq(&ep->lock, irqf);
            return -ENOENT;
        }
        break;
    }
    case EPOLL_CTL_DEL: {
        int found = -1;
        for (int i = 0; i < ep->count; i++) {
            if (ep->entries[i].fd == fd) { found = i; break; }
        }
        if (found < 0) {
            spin_unlock_irq(&ep->lock, irqf);
            return -ENOENT;
        }
        ep->entries[found] = ep->entries[ep->count - 1];
        ep->count--;
        break;
    }
    default:
        spin_unlock_irq(&ep->lock, irqf);
        return -EINVAL;
    }

    spin_unlock_irq(&ep->lock, irqf);
    return 0;
}

/* ── SYS_EPOLL_WAIT (232) ───────────────────────── */

long do_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
    if (maxevents <= 0) return -EINVAL;
    if (!events || !user_ok((uint64_t)events, (size_t)maxevents * sizeof(*events)))
        return -EFAULT;

    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *epfde = fd_get(&p->fds, epfd);
    if (!epfde || epfde->type != FD_EPOLL) return -EBADF;
    epoll_t *ep = (epoll_t *)epfde->obj;
    if (!ep) return -EBADF;

    uint64_t deadline = (timeout < 0)  ? (timer_ms() + 30000)
                      : (timeout == 0) ? 0
                      : (timer_ms() + (uint64_t)timeout);

    for (;;) {
        net_poll();

        int nready = 0;
        uint64_t irqf;
        spin_lock_irq(&ep->lock, &irqf);

        for (int i = 0; i < ep->count && nready < maxevents; i++) {
            uint32_t r = fd_poll_readiness(ep->entries[i].fd,
                                           ep->entries[i].events);
            if (r) {
                struct epoll_event ev;
                ev.events = r & ep->entries[i].events;
                /* Always report HUP/ERR even if not requested */
                ev.events |= r & (EPOLLHUP | EPOLLERR);
                if (ev.events) {
                    ev.data = ep->entries[i].data;
                    kmemcpy(&events[nready], &ev, sizeof(ev));
                    nready++;
                }
            }
        }

        spin_unlock_irq(&ep->lock, irqf);

        if (nready > 0) return nready;
        if (timeout == 0) return 0;
        if (timer_ms() >= deadline) return 0;

        /* Block: enable interrupts and halt until next IRQ */
        __asm__ volatile("sti; hlt");
    }
}

void epoll_destroy(void *obj) {
    if (obj) slab_free(&epoll_slab, obj);
}

/* ── SYS_EVENTFD2 (290) ─────────────────────────── */

long do_eventfd2(unsigned int initval, int flags) {
    (void)flags;
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    eventfd_t *efd = (eventfd_t *)slab_alloc(&eventfd_slab);
    if (!efd) return -ENOMEM;

    efd->counter = initval;
    efd->flags = flags;
    efd->lock = (spinlock_t)SPINLOCK_INIT;

    int fd = fd_alloc(&p->fds, FD_EVENTFD, efd, O_RDWR);
    if (fd < 0) {
        slab_free(&eventfd_slab, efd);
        return -EMFILE;
    }
    return fd;
}

long eventfd_read(void *obj, void *buf, long count) {
    if (count < 8) return -EINVAL;
    eventfd_t *efd = (eventfd_t *)obj;
    if (!efd) return -EBADF;

    uint64_t irqf;
    spin_lock_irq(&efd->lock, &irqf);
    if (efd->counter == 0) {
        spin_unlock_irq(&efd->lock, irqf);
        return -EAGAIN;
    }
    uint64_t val = efd->counter;
    efd->counter = 0;
    spin_unlock_irq(&efd->lock, irqf);

    kmemcpy(buf, &val, sizeof(val));
    return (long)sizeof(val);
}

long eventfd_write(void *obj, const void *buf, long count) {
    if (count < 8) return -EINVAL;
    eventfd_t *efd = (eventfd_t *)obj;
    if (!efd) return -EBADF;

    uint64_t val;
    kmemcpy(&val, buf, sizeof(val));

    uint64_t irqf;
    spin_lock_irq(&efd->lock, &irqf);
    if (efd->counter > 0xFFFFFFFFFFFFFFFEULL - val) {
        spin_unlock_irq(&efd->lock, irqf);
        return -EAGAIN;
    }
    efd->counter += val;
    spin_unlock_irq(&efd->lock, irqf);

    return (long)sizeof(val);
}

void eventfd_destroy(void *obj) {
    if (obj) slab_free(&eventfd_slab, obj);
}

/* ── SYS_TIMERFD_CREATE (283) ────────────────────── */

long do_timerfd_create(int clockid, int flags) {
    (void)clockid; (void)flags;
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    timerfd_t *tfd = (timerfd_t *)slab_alloc(&timerfd_slab);
    if (!tfd) return -ENOMEM;

    tfd->expire_ms = 0;
    tfd->interval_ms = 0;
    tfd->expirations = 0;
    tfd->armed = 0;
    tfd->flags = flags;
    tfd->lock = (spinlock_t)SPINLOCK_INIT;

    int fd = fd_alloc(&p->fds, FD_TIMERFD, tfd, O_RDWR);
    if (fd < 0) {
        slab_free(&timerfd_slab, tfd);
        return -EMFILE;
    }
    return fd;
}

static uint64_t ts_to_ms(const struct k_timespec *ts) {
    return (uint64_t)ts->tv_sec * 1000 + (uint64_t)ts->tv_nsec / 1000000;
}

static void ms_to_ts(uint64_t ms, struct k_timespec *ts) {
    ts->tv_sec = (long)(ms / 1000);
    ts->tv_nsec = (long)((ms % 1000) * 1000000);
}

long do_timerfd_settime(int fd, int tfd_flags,
                        const struct k_itimerspec *new_value,
                        struct k_itimerspec *old_value) {
    if (!new_value || !user_ok((uint64_t)new_value, sizeof(*new_value)))
        return -EFAULT;
    if (old_value && !user_ok((uint64_t)old_value, sizeof(*old_value)))
        return -EFAULT;

    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_TIMERFD) return -EBADF;
    timerfd_t *tfd = (timerfd_t *)fde->obj;
    if (!tfd) return -EBADF;

    struct k_itimerspec knew;
    kmemcpy(&knew, new_value, sizeof(knew));

    uint64_t irqf;
    spin_lock_irq(&tfd->lock, &irqf);

    if (old_value) {
        struct k_itimerspec kold;
        if (tfd->armed) {
            uint64_t now = timer_ms();
            uint64_t remaining = (tfd->expire_ms > now) ? (tfd->expire_ms - now) : 0;
            ms_to_ts(remaining, &kold.it_value);
            ms_to_ts(tfd->interval_ms, &kold.it_interval);
        } else {
            kold.it_value.tv_sec = 0;
            kold.it_value.tv_nsec = 0;
            kold.it_interval.tv_sec = 0;
            kold.it_interval.tv_nsec = 0;
        }
        kmemcpy(old_value, &kold, sizeof(kold));
    }

    uint64_t val_ms = ts_to_ms(&knew.it_value);
    uint64_t int_ms = ts_to_ms(&knew.it_interval);

    if (val_ms == 0 && int_ms == 0) {
        tfd->armed = 0;
        tfd->expirations = 0;
    } else {
        if (tfd_flags & 1) /* TFD_TIMER_ABSTIME */
            tfd->expire_ms = val_ms;
        else
            tfd->expire_ms = timer_ms() + val_ms;
        tfd->interval_ms = int_ms;
        tfd->expirations = 0;
        tfd->armed = 1;
    }

    spin_unlock_irq(&tfd->lock, irqf);
    return 0;
}

long timerfd_read(void *obj, void *buf, long count) {
    if (count < 8) return -EINVAL;
    timerfd_t *tfd = (timerfd_t *)obj;
    if (!tfd) return -EBADF;

    uint64_t irqf;
    spin_lock_irq(&tfd->lock, &irqf);

    if (tfd->armed) {
        uint64_t now = timer_ms();
        while (tfd->armed && now >= tfd->expire_ms) {
            tfd->expirations++;
            if (tfd->interval_ms > 0)
                tfd->expire_ms += tfd->interval_ms;
            else
                tfd->armed = 0;
        }
    }

    if (tfd->expirations == 0) {
        spin_unlock_irq(&tfd->lock, irqf);
        return -EAGAIN;
    }

    uint64_t val = tfd->expirations;
    tfd->expirations = 0;
    spin_unlock_irq(&tfd->lock, irqf);

    kmemcpy(buf, &val, sizeof(val));
    return (long)sizeof(val);
}

void timerfd_destroy(void *obj) {
    if (obj) slab_free(&timerfd_slab, obj);
}

/* ── SYS_SIGNALFD4 (289) — stub ─────────────────── */

long do_signalfd4(int fd, const uint64_t *mask, int flags) {
    (void)fd; (void)mask; (void)flags;
    return -ENOSYS;
}

/* ── Inotify string helpers ──────────────────────── */

static int ino_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void ino_strncpy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int ino_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a++ != *b++) return 0; }
    return *a == *b;
}

/* Extract parent directory path and basename from a full path.
 * Returns 1 on success, 0 on failure. */
static int ino_split_path(const char *path, char *dir, int dir_max,
                           const char **basename) {
    int len = ino_strlen(path);
    if (len == 0) return 0;
    int last_slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') { last_slash = i; break; }
    }
    if (last_slash < 0) return 0;
    *basename = path + last_slash + 1;
    if (!**basename) return 0;
    if (last_slash == 0) {
        dir[0] = '/'; dir[1] = 0;
    } else {
        int cplen = last_slash < dir_max - 1 ? last_slash : dir_max - 1;
        for (int i = 0; i < cplen; i++) dir[i] = path[i];
        dir[cplen] = 0;
    }
    return 1;
}

/* Queue an inotify event on all matching instances+watches.
 * Called from VFS operations. IRQ-safe (uses spin_lock_irq). */
void inotify_event(const char *path, uint32_t mask) {
    char dir[256];
    const char *basename;
    if (!ino_split_path(path, dir, 256, &basename)) return;

    for (int i = 0; i < INOTIFY_POOL_MAX; i++) {
        inotify_t *ino = &inotify_pool[i];
        /* Quick check: skip unallocated (watch_count == 0 and next_wd == 0) */
        uint64_t flags;
        spin_lock_irq(&ino->lock, &flags);
        for (int w = 0; w < ino->watch_count; w++) {
            if (!ino->watches[w].active) continue;
            if (!(ino->watches[w].mask & mask)) continue;
            if (!ino_streq(ino->watches[w].path, dir)) continue;

            /* Queue event */
            int next_tail = (ino->event_tail + 1) % INOTIFY_EVENT_MAX;
            if (next_tail == ino->event_head) {
                /* Overflow: drop oldest */
                ino->event_head = (ino->event_head + 1) % INOTIFY_EVENT_MAX;
            }
            inotify_queued_event_t *ev = &ino->events[ino->event_tail];
            ev->wd = ino->watches[w].wd;
            ev->mask = mask;
            ino_strncpy(ev->name, basename, 256);
            ino->event_tail = next_tail;
        }
        spin_unlock_irq(&ino->lock, flags);
    }
}

/* Check if inotify instance has pending events */
int inotify_has_events(void *obj) {
    inotify_t *ino = (inotify_t *)obj;
    if (!ino) return 0;
    return ino->event_head != ino->event_tail;
}

/* Read inotify events into user buffer. Returns bytes read or -EAGAIN. */
long inotify_read(void *obj, void *buf, long count) {
    inotify_t *ino = (inotify_t *)obj;
    if (!ino) return -EBADF;

    uint64_t flags;
    spin_lock_irq(&ino->lock, &flags);

    if (ino->event_head == ino->event_tail) {
        spin_unlock_irq(&ino->lock, flags);
        return -EAGAIN;
    }

    /* Linux inotify_event layout:
     * int wd (4), uint32_t mask (4), uint32_t cookie (4),
     * uint32_t len (4), char name[len] (padded to 4-byte boundary) */
    uint8_t *dst = (uint8_t *)buf;
    long written = 0;

    while (ino->event_head != ino->event_tail) {
        inotify_queued_event_t *ev = &ino->events[ino->event_head];
        int namelen = ino_strlen(ev->name) + 1; /* include NUL */
        int padded = (namelen + 3) & ~3;        /* align to 4 bytes */
        int total = 16 + padded;                /* header + name */

        if (written + total > count) break;     /* won't fit */

        /* Write header: wd, mask, cookie, len */
        uint32_t hdr[4];
        hdr[0] = (uint32_t)ev->wd;
        hdr[1] = ev->mask;
        hdr[2] = 0;                /* cookie (0 for now) */
        hdr[3] = (uint32_t)padded;
        kmemcpy(dst + written, hdr, 16);
        written += 16;

        /* Write name (zero-padded) */
        kmemset(dst + written, 0, (size_t)padded);
        kmemcpy(dst + written, ev->name, (size_t)(namelen - 1));
        /* NUL already set by kmemset */
        written += padded;

        ino->event_head = (ino->event_head + 1) % INOTIFY_EVENT_MAX;
    }

    spin_unlock_irq(&ino->lock, flags);
    return written > 0 ? written : -EAGAIN;
}

/* ── SYS_INOTIFY ────────────────────────────────── */

long do_inotify_init1(int flags) {
    (void)flags;
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    inotify_t *ino = (inotify_t *)slab_alloc(&inotify_slab);
    if (!ino) return -ENOMEM;

    ino->watch_count = 0;
    ino->next_wd = 1;
    ino->event_head = 0;
    ino->event_tail = 0;
    ino->lock = (spinlock_t)SPINLOCK_INIT;
    for (int i = 0; i < INOTIFY_WATCH_MAX; i++)
        ino->watches[i].active = 0;

    int fd = fd_alloc(&p->fds, FD_INOTIFY, ino, O_RDONLY);
    if (fd < 0) {
        slab_free(&inotify_slab, ino);
        return -EMFILE;
    }
    return fd;
}

long do_inotify_add_watch(int fd, const char *path, uint32_t mask) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_INOTIFY) return -EBADF;
    inotify_t *ino = (inotify_t *)fde->obj;
    if (!ino) return -EBADF;

    /* Copy path from user — safe byte-by-byte copy with bounds check */
    char kpath[256];
    if (!user_ok((uint64_t)path, 1)) return -EFAULT;
    {
        int pi = 0;
        for (; pi < 255; pi++) {
            if ((uint64_t)(path + pi) >= 0x800000000000ULL) return -EFAULT;
            kpath[pi] = path[pi];
            if (kpath[pi] == '\0') break;
        }
        kpath[pi] = '\0';
    }

    uint64_t flags;
    spin_lock_irq(&ino->lock, &flags);

    /* Check for existing watch on same path — update mask */
    for (int i = 0; i < ino->watch_count; i++) {
        if (ino->watches[i].active && ino_streq(ino->watches[i].path, kpath)) {
            ino->watches[i].mask = mask;
            int wd = ino->watches[i].wd;
            spin_unlock_irq(&ino->lock, flags);
            return wd;
        }
    }

    /* Find free slot */
    if (ino->watch_count >= INOTIFY_WATCH_MAX) {
        spin_unlock_irq(&ino->lock, flags);
        return -ENOMEM;
    }
    int slot = -1;
    for (int i = 0; i < INOTIFY_WATCH_MAX; i++) {
        if (!ino->watches[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        spin_unlock_irq(&ino->lock, flags);
        return -ENOMEM;
    }

    ino->watches[slot].wd = ino->next_wd++;
    ino->watches[slot].mask = mask;
    ino_strncpy(ino->watches[slot].path, kpath, 256);
    ino->watches[slot].active = 1;
    ino->watch_count++;
    int wd = ino->watches[slot].wd;

    spin_unlock_irq(&ino->lock, flags);
    return wd;
}

long do_inotify_rm_watch(int fd, int wd) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_INOTIFY) return -EBADF;
    inotify_t *ino = (inotify_t *)fde->obj;
    if (!ino) return -EBADF;

    uint64_t flags;
    spin_lock_irq(&ino->lock, &flags);
    for (int i = 0; i < INOTIFY_WATCH_MAX; i++) {
        if (ino->watches[i].active && ino->watches[i].wd == wd) {
            ino->watches[i].active = 0;
            ino->watch_count--;
            spin_unlock_irq(&ino->lock, flags);
            return 0;
        }
    }
    spin_unlock_irq(&ino->lock, flags);
    return -EINVAL;
}

void inotify_destroy(void *obj) {
    if (obj) slab_free(&inotify_slab, obj);
}
