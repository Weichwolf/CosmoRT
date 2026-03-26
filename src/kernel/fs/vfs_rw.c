/* CosmoRT VFS — read, write, lseek, pread, pwrite */

#include "fs/vfs_internal.h"

/* ── File growth ─────────────────────────────────── */

int grow_file(struct vfs_node *node, size_t needed) {
    if (needed <= node->capacity) return 0;

    /* Round up to page boundary */
    size_t new_cap = (needed + 4095) & ~4095ULL;
    int new_pages = (int)(new_cap / 4096);
    if (new_pages <= 0) new_pages = 1;

    uint8_t *new_data = (uint8_t *)pages_alloc(new_pages);
    if (!new_data) return -ENOMEM;

    if (node->data && node->size > 0) {
        kmemcpy(new_data, node->data, node->size);
    }

    if (node->data) {
        int old_pages = (int)((node->capacity + 4095) / 4096);
        if (old_pages > 0) pages_free(node->data, old_pages);
    }

    node->data = new_data;
    node->capacity = new_cap;
    return 0;
}

/* ── Read/Write ──────────────────────────────────── */

long vfs_read(int fd, void *buf, size_t count) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;

    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;

    if (f->backend == VFS_BACKEND_COSMOFS) {
        if (f->type != VFS_FILE) return -EISDIR;
        /* TOCTOU fix: bounce through kernel buffer in 4KB chunks */
        uint8_t kbuf[4096];
        size_t total = 0;
        while (total < count) {
            size_t chunk = count - total;
            if (chunk > 4096) chunk = 4096;
            int rc = cosmofs_read(f->cosmofs_ino, kbuf,
                                  (size_t)f->offset + total, chunk);
            if (rc < 0) return total > 0 ? (long)total : rc;
            if (rc == 0) break;
            kmemcpy((uint8_t *)buf + total, kbuf, (size_t)rc);
            total += (size_t)rc;
            if ((size_t)rc < chunk) break;
        }
        if (f->offset > UINT64_MAX - (uint64_t)total) return -EINVAL;
        f->offset += (uint64_t)total;
        return (long)total;
    }

    /* ramfs */
    if (!f->node) return -EBADF;
    struct vfs_node *node = f->node;
    if (node->type != VFS_FILE) return -EISDIR;

    if (f->offset >= node->size) return 0;

    size_t avail = node->size - (size_t)f->offset;
    if (count > avail) count = avail;

    /* TOCTOU fix: bounce through kernel buffer in 4KB chunks */
    if (node->data) {
        uint8_t kbuf[4096];
        size_t done = 0;
        while (done < count) {
            size_t chunk = count - done;
            if (chunk > 4096) chunk = 4096;
            kmemcpy(kbuf, node->data + f->offset + done, chunk);
            kmemcpy((uint8_t *)buf + done, kbuf, chunk);
            done += chunk;
        }
    }

    if (f->offset > UINT64_MAX - (uint64_t)count) return -EINVAL;
    f->offset += count;
    return (long)count;
}

long vfs_pread(struct vfs_file *f, void *buf, size_t count, uint64_t offset) {
    if (!f) return -EBADF;

    if (f->backend == VFS_BACKEND_COSMOFS) {
        if (f->type != VFS_FILE) return -EISDIR;
        extern int cosmofs_read(uint64_t ino, void *buf, size_t offset, size_t len);
        int rc = cosmofs_read(f->cosmofs_ino, buf, (size_t)offset, count);
        return (long)rc;
    }

    /* ramfs */
    if (!f->node) return -EBADF;
    struct vfs_node *node = f->node;
    if (node->type != VFS_FILE) return -EISDIR;

    if (offset >= node->size) return 0;

    size_t avail = node->size - (size_t)offset;
    if (count > avail) count = avail;

    if (node->data)
        kmemcpy(buf, node->data + offset, count);

    return (long)count;
}

long vfs_pwrite(struct vfs_file *f, const void *buf, size_t count, uint64_t offset) {
    if (!f) return -EBADF;

    if (f->backend == VFS_BACKEND_COSMOFS) {
        if (f->type != VFS_FILE) return -EISDIR;
        uint8_t kbuf[4096];
        size_t total = 0;
        while (total < count) {
            size_t chunk = count - total;
            if (chunk > 4096) chunk = 4096;
            kmemcpy(kbuf, (const uint8_t *)buf + total, chunk);
            int rc = cosmofs_write(f->cosmofs_ino, kbuf,
                                   (size_t)offset + total, chunk);
            if (rc < 0) return total > 0 ? (long)total : rc;
            total += (size_t)rc;
            if ((size_t)rc < chunk) break;
        }
        return (long)total;
    }

    /* ramfs */
    if (!f->node) return -EBADF;
    struct vfs_node *node = f->node;
    if (node->type != VFS_FILE) return -EISDIR;

    size_t end = (size_t)offset + count;
    if (end > node->capacity) {
        if (grow_file(node, end) < 0) return -ENOMEM;
    }

    uint8_t kbuf[4096];
    size_t done = 0;
    while (done < count) {
        size_t chunk = count - done;
        if (chunk > 4096) chunk = 4096;
        kmemcpy(kbuf, (const uint8_t *)buf + done, chunk);
        kmemcpy(node->data + offset + done, kbuf, chunk);
        done += chunk;
    }
    if (end > node->size) node->size = end;

    vfs_notify_modify(node);
    return (long)count;
}

long vfs_write(int fd, const void *buf, size_t count) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;

    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;

    if (f->backend == VFS_BACKEND_COSMOFS) {
        if (f->type != VFS_FILE) return -EISDIR;
        if (f->flags & O_APPEND) {
            /* Re-read inode size for append */
            struct cosmofs_inode *ip = cosmofs_inode_read(f->cosmofs_ino);
            if (ip) f->offset = ip->size;
        }
        /* TOCTOU fix: bounce through kernel buffer in 4KB chunks */
        uint8_t kbuf[4096];
        size_t total = 0;
        while (total < count) {
            size_t chunk = count - total;
            if (chunk > 4096) chunk = 4096;
            kmemcpy(kbuf, (const uint8_t *)buf + total, chunk);
            int rc = cosmofs_write(f->cosmofs_ino, kbuf,
                                   (size_t)f->offset + total, chunk);
            if (rc < 0) return total > 0 ? (long)total : rc;
            total += (size_t)rc;
            if ((size_t)rc < chunk) break;
        }
        f->offset += (uint64_t)total;
        f->cosmofs_size = f->offset;
        return (long)total;
    }

    /* ramfs */
    if (!f->node) return -EBADF;
    struct vfs_node *node = f->node;
    if (node->type != VFS_FILE) return -EISDIR;

    if (f->flags & O_APPEND)
        f->offset = node->size;

    size_t end = (size_t)f->offset + count;
    if (end > node->capacity) {
        if (grow_file(node, end) < 0) return -ENOMEM;
    }

    /* TOCTOU fix: bounce through kernel buffer in 4KB chunks */
    {
        uint8_t kbuf[4096];
        size_t done = 0;
        while (done < count) {
            size_t chunk = count - done;
            if (chunk > 4096) chunk = 4096;
            kmemcpy(kbuf, (const uint8_t *)buf + done, chunk);
            kmemcpy(node->data + f->offset + done, kbuf, chunk);
            done += chunk;
        }
    }
    f->offset = end;
    if (end > node->size) node->size = end;

    vfs_notify_modify(node);
    return (long)count;
}

/* ── Read by inode (for demand paging in page fault handler) ── */

static struct vfs_node *ramfs_find_by_ino(struct vfs_node *node, uint64_t ino) {
    if (!node) return 0;
    if (node->ino == ino) return node;
    for (struct vfs_node *c = node->children; c; c = c->next) {
        struct vfs_node *r = ramfs_find_by_ino(c, ino);
        if (r) return r;
    }
    return 0;
}

long vfs_pread_by_ino(int backend, uint64_t ino, void *buf, size_t offset, size_t len) {
    if (backend == VFS_BACKEND_COSMOFS) {
        extern int cosmofs_read(uint64_t ino, void *buf, size_t offset, size_t len);
        return (long)cosmofs_read(ino, buf, offset, len);
    }
    /* ramfs: find node by inode, read from its data buffer */
    extern struct vfs_node *vfs_root_node;
    struct vfs_node *node = ramfs_find_by_ino(vfs_root_node, ino);
    if (!node || node->type != VFS_FILE) return -ENOENT;
    if (offset >= node->size) return 0;
    size_t avail = node->size - offset;
    if (len > avail) len = avail;
    if (node->data)
        kmemcpy(buf, node->data + offset, len);
    return (long)len;
}

long vfs_lseek(int fd, long offset, int whence) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;

    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;

    long new_off;
    if (f->backend == VFS_BACKEND_COSMOFS) {
        uint64_t sz = f->cosmofs_size;
        struct cosmofs_inode *ip = cosmofs_inode_read(f->cosmofs_ino);
        if (ip) sz = ip->size;
        switch (whence) {
        case SEEK_SET: new_off = offset; break;
        case SEEK_CUR: new_off = (long)f->offset + offset; break;
        case SEEK_END: new_off = (long)sz + offset; break;
        default: return -EINVAL;
        }
    } else {
        if (!f->node) return -EBADF;
        switch (whence) {
        case SEEK_SET: new_off = offset; break;
        case SEEK_CUR: new_off = (long)f->offset + offset; break;
        case SEEK_END: new_off = (long)f->node->size + offset; break;
        default: return -EINVAL;
        }
    }

    if (new_off < 0) return -EINVAL;
    f->offset = (uint64_t)new_off;
    return new_off;
}
