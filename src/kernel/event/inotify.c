/* CosmoRT inotify — file system event monitoring
 *
 * Real implementation: watches track path+mask, events queued in ring buffer,
 * delivered via read(). poll/epoll report POLLIN when events available.
 */

#include "event/epoll.h"
#include "proc/process.h"
#include "mm/slab.h"
#include "spinlock.h"
#include "event/fd.h"
#include "uaccess.h"
#include "memops.h"

/* ── Watch entry ──────────────────────────────────── */

#define INOTIFY_MAX_WATCHES  32
#define INOTIFY_EVENT_RING   2048  /* bytes — ring buffer for events */

typedef struct {
    int      wd;           /* watch descriptor (1-based, 0 = unused) */
    uint32_t mask;         /* event mask */
    char     path[256];    /* watched path (absolute) */
    int      active;       /* 1 = live, 0 = removed */
} inotify_watch_t;

/* ── Inotify instance ─────────────────────────────── */

typedef struct {
    inotify_watch_t watches[INOTIFY_MAX_WATCHES];
    int             next_wd;       /* next watch descriptor to assign */
    int             refcount;

    /* Event ring buffer */
    uint8_t         ring[INOTIFY_EVENT_RING];
    int             ring_head;     /* read position */
    int             ring_tail;     /* write position */
    int             ring_used;     /* bytes used */
    int             overflow;      /* 1 if overflow occurred */

    spinlock_t      lock;
} inotify_t;

/* ── Slab pool ────────────────────────────────────── */

#define INOTIFY_POOL_MAX 16

static inotify_t inotify_pool[INOTIFY_POOL_MAX];
static slab_t    inotify_slab;

void inotify_init_slab(void) {
    slab_init(&inotify_slab, inotify_pool, (int)sizeof(inotify_t), INOTIFY_POOL_MAX);
}

/* ── String helpers (local) ───────────────────────── */

static int ino_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

static int ino_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a++ != *b++) return 0; }
    return *a == *b;
}

static void ino_strcpy(char *dst, const char *src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Check if `child` is directly inside directory `dir`.
 * e.g. dir="/tmp", child="/tmp/foo" → match, basename="foo"
 *      dir="/tmp", child="/tmp/sub/foo" → no match
 *      dir="/tmp/foo", child="/tmp/foo" → no match (same path, not child)
 *
 * Returns pointer to basename within child, or NULL. */
static const char *is_direct_child(const char *dir, const char *child) {
    int dlen = ino_strlen(dir);
    /* Strip trailing slash from dir for comparison */
    while (dlen > 1 && dir[dlen - 1] == '/') dlen--;

    /* child must start with dir + '/' */
    for (int i = 0; i < dlen; i++) {
        if (child[i] != dir[i]) return 0;
    }
    if (child[dlen] != '/') return 0;

    const char *base = &child[dlen + 1];
    if (!*base) return 0; /* trailing slash, no actual name */

    /* Must not contain another '/' (i.e. must be direct child) */
    for (const char *p = base; *p; p++) {
        if (*p == '/') return 0;
    }
    return base;
}

/* Extract parent directory path and basename from a full path.
 * Returns basename pointer within path, writes parent to buf. */
static const char *split_parent(const char *path, char *parent, int pmax) {
    int len = ino_strlen(path);
    /* Find last '/' */
    int last_slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') { last_slash = i; break; }
    }
    if (last_slash <= 0) {
        parent[0] = '/'; parent[1] = 0;
        return &path[1];
    }
    int copy = last_slash < pmax - 1 ? last_slash : pmax - 1;
    for (int i = 0; i < copy; i++) parent[i] = path[i];
    parent[copy] = 0;
    return &path[last_slash + 1];
}

/* ── Ring buffer operations (caller holds lock) ───── */

static int ring_free(inotify_t *ino) {
    return INOTIFY_EVENT_RING - ino->ring_used;
}

static void ring_write(inotify_t *ino, const void *data, int len) {
    const uint8_t *src = (const uint8_t *)data;
    for (int i = 0; i < len; i++) {
        ino->ring[ino->ring_tail] = src[i];
        ino->ring_tail = (ino->ring_tail + 1) % INOTIFY_EVENT_RING;
    }
    ino->ring_used += len;
}

static void ring_read(inotify_t *ino, void *data, int len) {
    uint8_t *dst = (uint8_t *)data;
    for (int i = 0; i < len; i++) {
        dst[i] = ino->ring[ino->ring_head];
        ino->ring_head = (ino->ring_head + 1) % INOTIFY_EVENT_RING;
    }
    ino->ring_used -= len;
}

/* Peek at bytes without consuming */
static void ring_peek(inotify_t *ino, void *data, int len) {
    uint8_t *dst = (uint8_t *)data;
    int pos = ino->ring_head;
    for (int i = 0; i < len; i++) {
        dst[i] = ino->ring[pos];
        pos = (pos + 1) % INOTIFY_EVENT_RING;
    }
}

/* ── Queue one event into an inotify instance (lock held) ── */

static void queue_event(inotify_t *ino, int wd, uint32_t mask,
                        uint32_t cookie, const char *name) {
    /* Build the event: header + name (padded to 4-byte boundary) */
    int name_len = name ? ino_strlen(name) : 0;
    /* len field: includes null terminator + padding to 4-byte alignment */
    uint32_t padded_len = 0;
    if (name_len > 0)
        padded_len = (uint32_t)((name_len + 1 + 3) & ~3);

    int total = 16 + (int)padded_len; /* sizeof header = 16 (wd + mask + cookie + len) */

    if (total > ring_free(ino)) {
        /* Queue overflow — set flag, will generate IN_Q_OVERFLOW on next read */
        ino->overflow = 1;
        return;
    }

    /* Write header fields individually (struct inotify_event layout) */
    int32_t k_wd = (int32_t)wd;
    ring_write(ino, &k_wd, 4);
    ring_write(ino, &mask, 4);
    ring_write(ino, &cookie, 4);
    ring_write(ino, &padded_len, 4);

    /* Write name + null + padding */
    if (padded_len > 0) {
        /* Write name bytes */
        ring_write(ino, name, name_len);
        /* Write null + padding (padded_len - name_len zeros) */
        uint8_t zeros[4] = {0, 0, 0, 0};
        int pad = (int)padded_len - name_len;
        ring_write(ino, zeros, pad);
    }
}

/* ── SYS_INOTIFY_INIT1 (294) ─────────────────────── */

long do_inotify_init1(int flags) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    inotify_t *ino = (inotify_t *)slab_alloc(&inotify_slab);
    if (!ino) return -ENOMEM;

    ino->refcount = 1;
    ino->next_wd = 1;
    ino->ring_head = 0;
    ino->ring_tail = 0;
    ino->ring_used = 0;
    ino->overflow = 0;
    ino->lock = (spinlock_t)SPINLOCK_INIT;

    for (int i = 0; i < INOTIFY_MAX_WATCHES; i++)
        ino->watches[i].wd = 0;

    int fd_flags = O_RDWR;
    if (flags & O_CLOEXEC)  fd_flags |= O_CLOEXEC;
    if (flags & O_NONBLOCK) fd_flags |= O_NONBLOCK;

    int fd = fd_alloc(&p->fds, FD_INOTIFY, ino, fd_flags);
    if (fd < 0) {
        slab_free(&inotify_slab, ino);
        return -EMFILE;
    }
    return fd;
}

/* ── SYS_INOTIFY_ADD_WATCH (254) ─────────────────── */

long do_inotify_add_watch(int fd, const char *path, uint32_t mask) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_INOTIFY) return -EBADF;
    inotify_t *ino = (inotify_t *)fde->obj;
    if (!ino) return -EBADF;

    /* Copy path from userspace */
    char kpath[256];
    if (copy_from_user(kpath, path, 256) < 0) return -EFAULT;
    kpath[255] = 0;

    /* Normalize: strip trailing slash (except root) */
    int plen = ino_strlen(kpath);
    while (plen > 1 && kpath[plen - 1] == '/') kpath[--plen] = 0;

    uint64_t irqf;
    spin_lock_irq(&ino->lock, &irqf);

    /* Check if already watching this path — update mask (IN_MASK_ADD merges) */
    for (int i = 0; i < INOTIFY_MAX_WATCHES; i++) {
        if (ino->watches[i].wd && ino->watches[i].active &&
            ino_streq(ino->watches[i].path, kpath)) {
            if (mask & IN_MASK_ADD)
                ino->watches[i].mask |= (mask & ~IN_MASK_ADD);
            else
                ino->watches[i].mask = mask;
            int wd = ino->watches[i].wd;
            spin_unlock_irq(&ino->lock, irqf);
            return wd;
        }
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < INOTIFY_MAX_WATCHES; i++) {
        if (ino->watches[i].wd == 0) { slot = i; break; }
    }
    if (slot < 0) {
        spin_unlock_irq(&ino->lock, irqf);
        return -ENOSPC;
    }

    int wd = ino->next_wd++;
    ino->watches[slot].wd = wd;
    ino->watches[slot].mask = mask;
    ino->watches[slot].active = 1;
    ino_strcpy(ino->watches[slot].path, kpath, 256);

    spin_unlock_irq(&ino->lock, irqf);
    return wd;
}

/* ── SYS_INOTIFY_RM_WATCH (255) ──────────────────── */

long do_inotify_rm_watch(int fd, int wd) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_INOTIFY) return -EBADF;
    inotify_t *ino = (inotify_t *)fde->obj;
    if (!ino) return -EBADF;

    uint64_t irqf;
    spin_lock_irq(&ino->lock, &irqf);

    int found = 0;
    for (int i = 0; i < INOTIFY_MAX_WATCHES; i++) {
        if (ino->watches[i].wd == wd && ino->watches[i].active) {
            /* Queue IN_IGNORED event before removing */
            queue_event(ino, wd, IN_IGNORED, 0, 0);
            ino->watches[i].wd = 0;
            ino->watches[i].active = 0;
            found = 1;
            break;
        }
    }

    spin_unlock_irq(&ino->lock, irqf);

    if (found) epoll_wake_all();
    return found ? 0 : -EINVAL;
}

/* ── read() on inotify fd ─────────────────────────── */

long inotify_read(void *obj, void *buf, long count) {
    inotify_t *ino = (inotify_t *)obj;
    if (!ino) return -EBADF;
    if (count < 16) return -EINVAL; /* must fit at least one header */

    uint64_t irqf;
    spin_lock_irq(&ino->lock, &irqf);

    /* If overflow, inject a synthetic IN_Q_OVERFLOW event */
    if (ino->overflow && ino->ring_used == 0) {
        int32_t k_wd = -1;
        uint32_t k_mask = IN_Q_OVERFLOW;
        uint32_t k_cookie = 0;
        uint32_t k_len = 0;
        /* Write directly — we know ring is empty */
        ring_write(ino, &k_wd, 4);
        ring_write(ino, &k_mask, 4);
        ring_write(ino, &k_cookie, 4);
        ring_write(ino, &k_len, 4);
        ino->overflow = 0;
    }

    if (ino->ring_used == 0) {
        spin_unlock_irq(&ino->lock, irqf);
        return -EAGAIN;
    }

    /* Read as many complete events as fit in buf */
    long total = 0;
    uint8_t *out = (uint8_t *)buf;

    while (ino->ring_used >= 16 && total + 16 <= count) {
        /* Peek at the header to get event size */
        uint8_t hdr[16];
        ring_peek(ino, hdr, 16);
        uint32_t name_len;
        kmemcpy(&name_len, &hdr[12], 4);
        int ev_size = 16 + (int)name_len;

        if (ev_size > ino->ring_used) break; /* corrupt — shouldn't happen */
        if (total + ev_size > count) break;   /* won't fit in user buf */

        /* Read full event to user buffer */
        uint8_t ev_buf[16 + 260]; /* max event: header + 256 name + padding */
        if (ev_size > (int)sizeof(ev_buf)) break;
        ring_read(ino, ev_buf, ev_size);
        copy_to_user(out + total, ev_buf, (size_t)ev_size);
        total += ev_size;
    }

    spin_unlock_irq(&ino->lock, irqf);
    return total > 0 ? total : -EAGAIN;
}

/* ── Poll readiness ───────────────────────────────── */

int inotify_has_events(void *obj) {
    inotify_t *ino = (inotify_t *)obj;
    if (!ino) return 0;
    return ino->ring_used > 0 || ino->overflow;
}

/* ── Event generation (called from VFS hooks) ─────── */

static uint32_t cookie_counter;

void inotify_event(const char *path, uint32_t mask) {
    if (!path) return;

    /* Compute cookie for rename pairs */
    uint32_t cookie = 0;
    if (mask & (IN_MOVED_FROM | IN_MOVED_TO))
        cookie = (mask & IN_MOVED_FROM)
               ? __sync_add_and_fetch(&cookie_counter, 1)
               : cookie_counter; /* MOVED_TO uses same cookie as preceding MOVED_FROM */

    /* Decompose path into parent + basename for directory watches */
    (void)split_parent;

    int woke = 0;

    /* Scan ALL inotify instances for matching watches */
    for (int p = 0; p < INOTIFY_POOL_MAX; p++) {
        inotify_t *ino = &inotify_pool[p];
        if (ino->refcount <= 0) continue;

        uint64_t irqf;
        spin_lock_irq(&ino->lock, &irqf);

        for (int w = 0; w < INOTIFY_MAX_WATCHES; w++) {
            inotify_watch_t *watch = &ino->watches[w];
            if (!watch->wd || !watch->active) continue;

            /* Case 1: Watch on the exact file/dir that changed */
            if (ino_streq(watch->path, path)) {
                if (watch->mask & mask) {
                    queue_event(ino, watch->wd, mask, cookie, 0);
                    woke = 1;
                    if (watch->mask & IN_ONESHOT) {
                        watch->wd = 0;
                        watch->active = 0;
                    }
                }
            }
            /* Case 2: Watch on parent directory — child event */
            else {
                const char *child_name = is_direct_child(watch->path, path);
                if (child_name && (watch->mask & mask)) {
                    queue_event(ino, watch->wd, mask, cookie, child_name);
                    woke = 1;
                    if (watch->mask & IN_ONESHOT) {
                        watch->wd = 0;
                        watch->active = 0;
                    }
                }
            }
        }

        spin_unlock_irq(&ino->lock, irqf);
    }

    if (woke) epoll_wake_all();
}

/* ── Refcount management ──────────────────────────── */

void inotify_incref(void *obj) {
    if (!obj) return;
    inotify_t *ino = (inotify_t *)obj;
    __sync_add_and_fetch(&ino->refcount, 1);
}

void inotify_destroy(void *obj) {
    if (!obj) return;
    inotify_t *ino = (inotify_t *)obj;
    if (__sync_sub_and_fetch(&ino->refcount, 1) <= 0)
        slab_free(&inotify_slab, obj);
}
