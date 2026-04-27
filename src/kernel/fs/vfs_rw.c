/* CosmoRT VFS — read, write, lseek, pread, pwrite
 *
 * Dispatches through f->f_ops when set, falls back to legacy backend check.
 */

#include "fs/vfs_internal.h"
#include "linux/signal.h"

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

/* ── File growth (tmpfs) ────────────────────────── */

int grow_file(struct vfs_inode *inode, size_t needed) {
    if (needed <= inode->capacity) return 0;
    size_t new_cap = (needed + 4095) & ~4095ULL;
    int new_pages = (int)(new_cap / 4096);
    if (new_pages <= 0) new_pages = 1;
    uint8_t *new_data = (uint8_t *)pages_alloc(new_pages);
    if (!new_data) return -ENOMEM;
    if (inode->data && inode->size > 0)
        kmemcpy(new_data, inode->data, inode->size);
    if (inode->data) {
        int old_pages = (int)((inode->capacity + 4095) / 4096);
        if (old_pages > 0) pages_free(inode->data, old_pages);
    }
    inode->data = new_data;
    inode->capacity = new_cap;
    return 0;
}

/* ── Read/Write — dispatch via f_ops ────────────── */

long vfs_read(int fd, void *buf, size_t count) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
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
    fd_entry_t *fde = fd_get(&p->fds, fd);
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
    fd_entry_t *fde = fd_get(&p->fds, fd);
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
    /* tmpfs: find inode, read from its data buffer */
    struct vfs_inode *inode = ramfs_find_by_ino(vfs_root_node, ino);
    if (!inode || inode->type != VFS_FILE) return -ENOENT;
    if (offset >= inode->size) return 0;
    size_t avail = inode->size - offset;
    if (len > avail) len = avail;
    if (inode->data)
        kmemcpy(buf, inode->data + offset, len);
    return (long)len;
}

long vfs_pwrite_by_ino(int backend, uint64_t ino, const void *buf, size_t offset, size_t len) {
    if (backend == VFS_BACKEND_EXT4)
        return (long)ext4_write((uint32_t)ino, buf, offset, len);
    struct vfs_inode *inode = ramfs_find_by_ino(vfs_root_node, ino);
    if (!inode || inode->type != VFS_FILE) return -ENOENT;
    size_t end = offset + len;
    if (end > inode->capacity) {
        if (grow_file(inode, end) < 0) return -ENOMEM;
    }
    if (inode->data)
        kmemcpy(inode->data + offset, buf, len);
    if (end > inode->size) inode->size = end;
    return (long)len;
}
