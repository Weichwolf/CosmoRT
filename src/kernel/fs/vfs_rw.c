/* CosmoRT VFS — read, write, lseek, pread, pwrite */

#include "fs/vfs_internal.h"

/* ── File growth ─────────────────────────────────── */

int grow_file(struct vfs_inode *inode, size_t needed) {
    if (needed <= inode->capacity) return 0;

    /* Round up to page boundary */
    size_t new_cap = (needed + 4095) & ~4095ULL;
    int new_pages = (int)(new_cap / 4096);
    if (new_pages <= 0) new_pages = 1;

    uint8_t *new_data = (uint8_t *)pages_alloc(new_pages);
    if (!new_data) return -ENOMEM;

    if (inode->data && inode->size > 0) {
        kmemcpy(new_data, inode->data, inode->size);
    }

    if (inode->data) {
        int old_pages = (int)((inode->capacity + 4095) / 4096);
        if (old_pages > 0) pages_free(inode->data, old_pages);
    }

    inode->data = new_data;
    inode->capacity = new_cap;
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

    if (f->backend == VFS_BACKEND_EXT2) {
        if (f->type != VFS_FILE) return -EISDIR;
        /* TOCTOU fix: bounce through kernel buffer in 4KB chunks */
        uint8_t kbuf[4096];
        size_t total = 0;
        while (total < count) {
            size_t chunk = count - total;
            if (chunk > 4096) chunk = 4096;
            int rc = ext2_read((uint32_t)f->disk_ino, kbuf,
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
    if (!f->inode) return -EBADF;
    struct vfs_inode *inode = f->inode;
    if (inode->type != VFS_FILE) return -EISDIR;

    if (f->offset >= inode->size) return 0;

    size_t avail = inode->size - (size_t)f->offset;
    if (count > avail) count = avail;

    /* TOCTOU fix: bounce through kernel buffer in 4KB chunks */
    if (inode->data) {
        uint8_t kbuf[4096];
        size_t done = 0;
        while (done < count) {
            size_t chunk = count - done;
            if (chunk > 4096) chunk = 4096;
            kmemcpy(kbuf, inode->data + f->offset + done, chunk);
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

    if (f->backend == VFS_BACKEND_EXT2) {
        if (f->type != VFS_FILE) return -EISDIR;
        int rc = ext2_read((uint32_t)f->disk_ino, buf, (size_t)offset, count);
        return (long)rc;
    }

    /* ramfs */
    if (!f->inode) return -EBADF;
    struct vfs_inode *inode = f->inode;
    if (inode->type != VFS_FILE) return -EISDIR;

    if (offset >= inode->size) return 0;

    size_t avail = inode->size - (size_t)offset;
    if (count > avail) count = avail;

    if (inode->data)
        kmemcpy(buf, inode->data + offset, count);

    return (long)count;
}

long vfs_pwrite(struct vfs_file *f, const void *buf, size_t count, uint64_t offset) {
    if (!f) return -EBADF;

    if (f->backend == VFS_BACKEND_EXT2) {
        if (f->type != VFS_FILE) return -EISDIR;
        uint8_t kbuf[4096];
        size_t total = 0;
        while (total < count) {
            size_t chunk = count - total;
            if (chunk > 4096) chunk = 4096;
            kmemcpy(kbuf, (const uint8_t *)buf + total, chunk);
            int rc = ext2_write((uint32_t)f->disk_ino, kbuf,
                                (size_t)offset + total, chunk);
            if (rc < 0) return total > 0 ? (long)total : rc;
            total += (size_t)rc;
            if ((size_t)rc < chunk) break;
        }
        if (total > 0 && f->path[0])
            inotify_event(f->path, IN_MODIFY);
        return (long)total;
    }

    /* ramfs */
    if (!f->inode) return -EBADF;
    struct vfs_inode *inode = f->inode;
    if (inode->type != VFS_FILE) return -EISDIR;

    size_t end = (size_t)offset + count;
    if (end > inode->capacity) {
        if (grow_file(inode, end) < 0) return -ENOMEM;
    }

    uint8_t kbuf[4096];
    size_t done = 0;
    while (done < count) {
        size_t chunk = count - done;
        if (chunk > 4096) chunk = 4096;
        kmemcpy(kbuf, (const uint8_t *)buf + done, chunk);
        kmemcpy(inode->data + offset + done, kbuf, chunk);
        done += chunk;
    }
    if (end > inode->size) inode->size = end;
    { extern uint32_t timer_epoch_sec(void); inode->mtime = timer_epoch_sec(); }

    if (f->path[0])
        inotify_event(f->path, IN_MODIFY);
    return (long)count;
}

long vfs_write(int fd, const void *buf, size_t count) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;

    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;

    if (f->backend == VFS_BACKEND_EXT2) {
        if (f->type != VFS_FILE) return -EISDIR;
        if (f->flags & O_APPEND) {
            /* Re-read inode size for append */
            struct ext2_inode ip;
            if (ext2_inode_read((uint32_t)f->disk_ino, &ip) == 0)
                f->offset = ip.i_size;
        }
        /* TOCTOU fix: bounce through kernel buffer in 4KB chunks */
        uint8_t kbuf[4096];
        size_t total = 0;
        while (total < count) {
            size_t chunk = count - total;
            if (chunk > 4096) chunk = 4096;
            kmemcpy(kbuf, (const uint8_t *)buf + total, chunk);
            int rc = ext2_write((uint32_t)f->disk_ino, kbuf,
                                (size_t)f->offset + total, chunk);
            if (rc < 0) return total > 0 ? (long)total : rc;
            total += (size_t)rc;
            if ((size_t)rc < chunk) break;
        }
        f->offset += (uint64_t)total;
        f->disk_size = f->offset;
        /* Invalidate page cache — demand-paged readers must see new data */
        extern void page_cache_invalidate_ino(uint64_t ino);
        page_cache_invalidate_ino(f->disk_ino);
        if (total > 0 && f->path[0])
            inotify_event(f->path, IN_MODIFY);
        return (long)total;
    }

    /* ramfs */
    if (!f->inode) return -EBADF;
    struct vfs_inode *inode = f->inode;
    if (inode->type != VFS_FILE) return -EISDIR;

    if (f->flags & O_APPEND)
        f->offset = inode->size;

    size_t end = (size_t)f->offset + count;
    if (end > inode->capacity) {
        if (grow_file(inode, end) < 0) return -ENOMEM;
    }

    /* TOCTOU fix: bounce through kernel buffer in 4KB chunks */
    {
        uint8_t kbuf[4096];
        size_t done = 0;
        while (done < count) {
            size_t chunk = count - done;
            if (chunk > 4096) chunk = 4096;
            kmemcpy(kbuf, (const uint8_t *)buf + done, chunk);
            kmemcpy(inode->data + f->offset + done, kbuf, chunk);
            done += chunk;
        }
    }
    f->offset = end;
    if (end > inode->size) inode->size = end;

    if (f->path[0])
        inotify_event(f->path, IN_MODIFY);
    /* Invalidate page cache — demand-paged readers must see new data */
    extern void page_cache_invalidate_ino(uint64_t ino);
    page_cache_invalidate_ino(inode->ino);
    return (long)count;
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
    if (backend == VFS_BACKEND_EXT2) {
        return (long)ext2_read((uint32_t)ino, buf, offset, len);
    }
    /* ramfs: find inode, read from its data buffer */
    extern struct vfs_node *vfs_root_node;
    struct vfs_inode *inode = ramfs_find_by_ino(vfs_root_node, ino);
    if (!inode || inode->type != VFS_FILE) return -ENOENT;
    if (offset >= inode->size) return 0;
    size_t avail = inode->size - offset;
    if (len > avail) len = avail;
    if (inode->data)
        kmemcpy(buf, inode->data + offset, len);
    return (long)len;
}

/* Write by inode (for msync / dirty page write-back) */

long vfs_pwrite_by_ino(int backend, uint64_t ino, const void *buf, size_t offset, size_t len) {
    if (backend == VFS_BACKEND_EXT2) {
        return (long)ext2_write((uint32_t)ino, buf, offset, len);
    }
    /* ramfs: find inode, write to its data buffer */
    extern struct vfs_node *vfs_root_node;
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

long vfs_lseek(int fd, long offset, int whence) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;

    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;

    long new_off;
    if (f->backend == VFS_BACKEND_EXT2) {
        uint64_t sz = f->disk_size;
        struct ext2_inode ip;
        if (ext2_inode_read((uint32_t)f->disk_ino, &ip) == 0)
            sz = ip.i_size;
        switch (whence) {
        case SEEK_SET: new_off = offset; break;
        case SEEK_CUR: new_off = (long)f->offset + offset; break;
        case SEEK_END: new_off = (long)sz + offset; break;
        default: return -EINVAL;
        }
    } else {
        if (!f->inode) return -EBADF;
        switch (whence) {
        case SEEK_SET: new_off = offset; break;
        case SEEK_CUR: new_off = (long)f->offset + offset; break;
        case SEEK_END: new_off = (long)f->inode->size + offset; break;
        default: return -EINVAL;
        }
    }

    if (new_off < 0) return -EINVAL;
    f->offset = (uint64_t)new_off;
    return new_off;
}
