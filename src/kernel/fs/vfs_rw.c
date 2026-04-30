/* CosmoRT VFS — read, write, lseek, pread, pwrite
 *
 * Dispatches through f->f_ops when set, falls back to legacy backend check.
 *
 * tmpfs/ramfs storage is a per-inode array of independently-allocated 4KB
 * pages (see struct vfs_inode { uint8_t **pages; ... }). No contiguous-page
 * requirement, so file size is bounded by available memory + the pages-array
 * container size (pages_alloc cap, currently 1GB → 256K entries × 8B = 2MB).
 *
 * grow_file/shrink_file mutate the page-list. ramfs_read/ramfs_write provide
 * flat r/w by walking the page array. inode->lock serializes mutations and
 * content access — held only across in-memory memcpy, never across I/O. */

#include "fs/vfs_internal.h"
#include "linux/signal.h"
#include "mm/page_alloc.h"

extern uint64_t *alloc_page(void);

/* ── tmpfs/ramfs page-budget ─────────────────────────────────────────────
 *
 * Linux's tmpfs caps total in-memory storage at 50% of RAM by default.
 * Without a cap, fill-the-FS tests (LTP fallocate05, file_attr05) wachsen
 * unbeschraenkt, bis OOM den ganzen Kernel kippt — kein eligible victim,
 * weil das tmpfs-storage nicht einem Prozess zuzuordnen ist.
 *
 * Wir tracken globalen tmpfs/ramfs-Page-Verbrauch. grow_file gibt
 * -ENOSPC zurueck wenn ein write das Budget sprengen wuerde — exakt das
 * Verhalten das LTP's ENOSPC-handler erwartet (`tst_fill_fs` halbiert len
 * und retried).
 *
 * Limit: 60% von total_pages. Default vom page_alloc-Total beim ersten
 * Aufruf abgefragt. Nicht per-mount (CosmoRT-tmpfs hat shared dentry-Tree),
 * aber per-prozess-OOM-resistent. */
static volatile int64_t ramfs_alloc_pages;
static volatile int64_t ramfs_max_pages;

static int64_t ramfs_get_max(void) {
    int64_t m = ramfs_max_pages;
    if (m == 0) {
        int total = page_alloc_total();
        m = (int64_t)total * 60 / 100;
        if (m <= 0) m = 1;
        ramfs_max_pages = m;
    }
    return m;
}

/* Reserve `add` pages from the ramfs budget. -ENOSPC if budget exhausted. */
static int ramfs_reserve(size_t add) {
    int64_t lim = ramfs_get_max();
    int64_t prev = __sync_fetch_and_add(&ramfs_alloc_pages, (int64_t)add);
    if (prev + (int64_t)add > lim) {
        __sync_sub_and_fetch(&ramfs_alloc_pages, (int64_t)add);
        return -ENOSPC;
    }
    return 0;
}

/* Return `n` pages to the ramfs budget. Called on shrink/destroy/rollback. */
static void ramfs_release(size_t n) {
    if (n) __sync_sub_and_fetch(&ramfs_alloc_pages, (int64_t)n);
}

/* Public wrapper for inode_destroy in vfs.c (different translation unit). */
void ramfs_release_pages(size_t n) { ramfs_release(n); }

/* RLIMIT_FSIZE enforcement (Linux semantics, write(2)):
 *   offset >= limit       → -EFBIG + SIGXFSZ (nothing written)
 *   offset + count > limit → count truncated to (limit - offset), partial OK
 * Returns adjusted count on success (may be 0 to indicate full block, -EFBIG).
 * Only regular files are subject to the limit (matches Linux). */
static long fsize_clamp_write(size_t *count_inout, uint64_t offset) {
    process_t *p = proc_current();
    if (!p) return 0;
    unsigned long lim = p->rlim_fsize;
    if (lim == (unsigned long)~0UL) return 0;
    if (offset >= lim) {
        extern long kill_one(process_t *target, int sig);
        kill_one(p, SIGXFSZ);
        return -EFBIG;
    }
    uint64_t room = (uint64_t)lim - offset;
    if (*count_inout > room) *count_inout = (size_t)room;
    return 0;
}

/* ── Page-list management ──────────────────────────
 *
 * Caller-side discipline:
 *   - grow_file/shrink_file/ramfs_*: caller must hold inode->lock.
 *     The exception: vfs_pread_by_ino / vfs_pwrite_by_ino take it themselves
 *     (they are entry points called from page-fault / mmap writeback paths).
 *   - For external callers (process_exec, vfs_kernel_append, vfs_add_file)
 *     vfs_inode_read provides a self-locking read variant. Writers in those
 *     contexts run before any concurrent open exists (single-threaded init
 *     populating ramfs) and use the unlocked helpers directly.
 */

/* Ensure pages-array container has at least `need` entries.
 * Grows by doubling, but capped at PAGES_ALLOC_MAX (2MB → 256K entries → 1GB).
 * Returns 0 on success, -ENOMEM on failure (old array still valid). */
static int pages_array_grow(struct vfs_inode *inode, size_t need) {
    if (need <= inode->pages_cap) return 0;

    size_t new_cap = inode->pages_cap ? inode->pages_cap : 16;
    while (new_cap < need) new_cap *= 2;

    /* Container size in 4KB pages, rounded up. */
    size_t bytes = new_cap * sizeof(uint8_t *);
    int n_container = (int)((bytes + 4095) / 4096);
    if (n_container > PAGES_ALLOC_MAX) {
        /* File would exceed 1GB hard cap — fail cleanly rather than truncate */
        return -ENOMEM;
    }
    /* pages_alloc rounds to power of 2; recompute new_cap to match the
     * actually-allocated container size so we don't waste. */
    int n_round = 1;
    while (n_round < n_container) n_round *= 2;
    size_t new_bytes = (size_t)n_round * 4096;
    new_cap = new_bytes / sizeof(uint8_t *);

    uint8_t **new_arr = (uint8_t **)pages_alloc(n_round);
    if (!new_arr) return -ENOMEM;

    if (inode->pages) {
        for (size_t i = 0; i < inode->npages; i++)
            new_arr[i] = inode->pages[i];
        size_t old_bytes = inode->pages_cap * sizeof(uint8_t *);
        int old_n = (int)((old_bytes + 4095) / 4096);
        int old_round = 1;
        while (old_round < old_n) old_round *= 2;
        if (old_round < 1) old_round = 1;
        pages_free(inode->pages, old_round);
    }
    inode->pages = new_arr;
    inode->pages_cap = new_cap;
    return 0;
}

/* ── File growth (tmpfs) ────────────────────────── */

int grow_file(struct vfs_inode *inode, size_t needed) {
    size_t need_pages = (needed + 4095) / 4096;
    if (need_pages <= inode->npages) return 0;

    int rc = pages_array_grow(inode, need_pages);
    if (rc < 0) return rc;

    size_t add = need_pages - inode->npages;
    /* Reserve global budget first so we never start allocating against a
     * full tmpfs and have to roll back many pages. ENOSPC propagates to
     * write/truncate which is exactly what LTP's tst_fill_fs expects. */
    int br = ramfs_reserve(add);
    if (br < 0) return br;

    size_t i = inode->npages;
    while (i < need_pages) {
        uint64_t *pg = alloc_page();
        if (!pg) {
            /* Roll back: free the pages we already allocated in this call,
             * release the entire reservation (allocated + unallocated). */
            for (size_t j = inode->npages; j < i; j++) {
                page_free(inode->pages[j]);
                inode->pages[j] = 0;
            }
            ramfs_release(add);
            return -ENOMEM;
        }
        /* alloc_page already zeroes the page. */
        inode->pages[i] = (uint8_t *)pg;
        i++;
    }
    inode->npages = need_pages;
    return 0;
}

/* Free pages above `new_size`. Caller updates inode->size separately. */
void shrink_file(struct vfs_inode *inode, size_t new_size) {
    size_t keep_pages = (new_size + 4095) / 4096;
    if (keep_pages >= inode->npages) return;
    size_t freed = inode->npages - keep_pages;
    for (size_t i = keep_pages; i < inode->npages; i++) {
        if (inode->pages[i]) {
            page_free(inode->pages[i]);
            inode->pages[i] = 0;
        }
    }
    inode->npages = keep_pages;
    ramfs_release(freed);
}

/* Flat read from page-list. Out-of-range bytes are zeroed (sparse semantics
 * inside [0, size); caller pre-clamped len so we don't run past npages). */
void ramfs_read(struct vfs_inode *inode, void *dst, size_t off, size_t len) {
    uint8_t *out = (uint8_t *)dst;
    size_t copied = 0;
    while (copied < len) {
        size_t pi = (off + copied) >> 12;
        size_t po = (off + copied) & 0xFFF;
        size_t chunk = 4096 - po;
        if (chunk > len - copied) chunk = len - copied;
        if (pi < inode->npages && inode->pages[pi]) {
            kmemcpy(out + copied, inode->pages[pi] + po, chunk);
        } else {
            kmemset(out + copied, 0, chunk);
        }
        copied += chunk;
    }
}

void ramfs_write(struct vfs_inode *inode, const void *src, size_t off, size_t len) {
    const uint8_t *in = (const uint8_t *)src;
    size_t copied = 0;
    while (copied < len) {
        size_t pi = (off + copied) >> 12;
        size_t po = (off + copied) & 0xFFF;
        size_t chunk = 4096 - po;
        if (chunk > len - copied) chunk = len - copied;
        /* Caller must have grown the inode first; assert via skip on missing. */
        if (pi < inode->npages && inode->pages[pi])
            kmemcpy(inode->pages[pi] + po, in + copied, chunk);
        copied += chunk;
    }
}

void ramfs_zero(struct vfs_inode *inode, size_t off, size_t len) {
    size_t done = 0;
    while (done < len) {
        size_t pi = (off + done) >> 12;
        size_t po = (off + done) & 0xFFF;
        size_t chunk = 4096 - po;
        if (chunk > len - done) chunk = len - done;
        if (pi < inode->npages && inode->pages[pi])
            kmemset(inode->pages[pi] + po, 0, chunk);
        done += chunk;
    }
}

size_t ramfs_read_locked(struct vfs_inode *inode, void *dst, size_t off, size_t len) {
    if (!inode || inode->type != VFS_FILE) return 0;
    uint64_t irqf;
    spin_lock_irq(&inode->lock, &irqf);
    size_t got = 0;
    if (off < inode->size) {
        size_t avail = inode->size - off;
        if (len > avail) len = avail;
        ramfs_read(inode, dst, off, len);
        got = len;
    }
    spin_unlock_irq(&inode->lock, irqf);
    return got;
}

/* Public entry point — used by callers without vfs_internal.h access. */
size_t vfs_inode_read(struct vfs_inode *inode, void *dst, size_t off, size_t len) {
    return ramfs_read_locked(inode, dst, off, len);
}

/* ── Read/Write — dispatch via f_ops ────────────── */

long vfs_read(int fd, void *buf, size_t count) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;
    if (f->f_ops && f->f_ops->read)
        return f->f_ops->read(f, buf, count);
    return -EBADF;
}

/* Effective write-offset for RLIMIT_FSIZE accounting.
 * O_APPEND makes the kernel (not user) choose the offset = current file size.
 * Using a stale f->offset would leak an append past the limit. */
static uint64_t effective_write_offset(struct vfs_file *f) {
    if (!(f->flags & O_APPEND)) return f->offset;
    if (f->inode) return (uint64_t)f->inode->size;
    if (f->disk_ino) return f->disk_size;
    return f->offset;
}

long vfs_write(int fd, const void *buf, size_t count) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;
    int ro = vfs_mount_writable(f->mnt);
    if (ro < 0) return ro;
    long clamp = fsize_clamp_write(&count, effective_write_offset(f));
    if (clamp < 0) return clamp;
    if (count == 0) return 0;
    if (f->f_ops && f->f_ops->write)
        return f->f_ops->write(f, buf, count);
    return -EBADF;
}

long vfs_lseek(int fd, long offset, int whence) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;
    if (f->f_ops && f->f_ops->lseek)
        return f->f_ops->lseek(f, offset, whence);
    return -ESPIPE;
}

long vfs_pread(struct vfs_file *f, void *buf, size_t count, uint64_t offset) {
    if (!f) return -EBADF;
    if (f->f_ops && f->f_ops->pread)
        return f->f_ops->pread(f, buf, count, offset);
    return -EBADF;
}

long vfs_pwrite(struct vfs_file *f, const void *buf, size_t count, uint64_t offset) {
    if (!f) return -EBADF;
    int ro = vfs_mount_writable(f->mnt);
    if (ro < 0) return ro;
    long clamp = fsize_clamp_write(&count, offset);
    if (clamp < 0) return clamp;
    if (count == 0) return 0;
    if (f->f_ops && f->f_ops->pwrite)
        return f->f_ops->pwrite(f, buf, count, offset);
    return -EBADF;
}

/* ── Read by inode (for demand paging in page fault handler) ── */

static struct vfs_inode *ramfs_find_by_ino(struct vfs_node *node, uint64_t ino) {
    if (!node || !node->inode) return 0;
    if (node->inode->ino == ino) return node->inode;
    for (struct vfs_node *c = node->inode->children; c; c = c->next) {
        struct vfs_inode *r = ramfs_find_by_ino(c, ino);
        if (r) return r;
    }
    return 0;
}

long vfs_pread_by_ino(int backend, uint64_t ino, void *buf, size_t offset, size_t len) {
    if (backend == VFS_BACKEND_EXT4)
        return (long)ext4_read((uint32_t)ino, buf, offset, len);
    /* tmpfs/ramfs: find inode, read from page-list (locked). */
    struct vfs_inode *inode = ramfs_find_by_ino(vfs_root_node, ino);
    if (!inode || inode->type != VFS_FILE) return -ENOENT;
    return (long)ramfs_read_locked(inode, buf, offset, len);
}

long vfs_pwrite_by_ino(int backend, uint64_t ino, const void *buf, size_t offset, size_t len) {
    if (backend == VFS_BACKEND_EXT4)
        return (long)ext4_write((uint32_t)ino, buf, offset, len);
    struct vfs_inode *inode = ramfs_find_by_ino(vfs_root_node, ino);
    if (!inode || inode->type != VFS_FILE) return -ENOENT;
    uint64_t irqf;
    spin_lock_irq(&inode->lock, &irqf);
    size_t end = offset + len;
    if (end > inode->size) {
        if (grow_file(inode, end) < 0) {
            spin_unlock_irq(&inode->lock, irqf);
            return -ENOMEM;
        }
    }
    ramfs_write(inode, buf, offset, len);
    if (end > inode->size) inode->size = end;
    spin_unlock_irq(&inode->lock, irqf);
    return (long)len;
}
