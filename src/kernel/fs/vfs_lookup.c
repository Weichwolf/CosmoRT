/* CosmoRT VFS — path lookup, resolution, traversal */

#include "fs/vfs_internal.h"

#define SYMLOOP_MAX 8

static struct vfs_node *vfs_lookup_impl(const char *path, int depth,
                                        int follow_last, int *err) {
    char buf[512];
    int blen = kstrlen(path);
    if (blen >= (int)sizeof(buf)) return 0;
    for (int i = 0; i <= blen; i++) buf[i] = path[i];

restart:
    if (depth > SYMLOOP_MAX) { if (err) *err = -ELOOP; return 0; }
    if (!buf[0]) return 0;
    if (buf[0] == '/' && buf[1] == 0) return vfs_root_node;

    struct vfs_node *cur = vfs_root_node;
    const char *p = buf;
    if (*p == '/') p++;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        const char *start = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - start);

        const char *rest = p;
        while (*rest == '/') rest++;
        int is_last = (*rest == 0);

        if (cur->type != VFS_DIR) { if (err) *err = -ENOTDIR; return 0; }

        struct vfs_node *child = cur->children;
        struct vfs_node *found = 0;
        while (child) {
            int nlen = kstrlen(child->name);
            if (nlen == len) {
                int match = 1;
                for (int i = 0; i < len; i++) {
                    if (child->name[i] != start[i]) { match = 0; break; }
                }
                if (match) { found = child; break; }
            }
            child = child->next;
        }
        if (!found) return 0;

        if (found->type == VFS_SYMLINK && (follow_last || !is_last)) {
            const char *target = found->symlink_target;
            if (!target[0]) return 0;
            depth++;

            int tlen = kstrlen(target);
            int rlen = kstrlen(p);
            if (tlen + 1 + rlen >= (int)sizeof(buf)) return 0;

            if (target[0] == '/') {
                for (int i = rlen; i >= 0; i--) buf[sizeof(buf) - 1 - rlen + i] = p[i];
                for (int i = 0; i < tlen; i++) buf[i] = target[i];
                int off = tlen;
                if (rlen > 0 && buf[off - 1] != '/') buf[off++] = '/';
                for (int i = 0; i < rlen; i++) buf[off + i] = buf[sizeof(buf) - 1 - rlen + i];
                buf[off + rlen] = 0;
            } else {
                int prefix_len = (int)(start - buf);
                for (int i = rlen; i >= 0; i--) buf[sizeof(buf) - 1 - rlen + i] = p[i];
                for (int i = 0; i < tlen; i++) buf[prefix_len + i] = target[i];
                int off = prefix_len + tlen;
                if (rlen > 0 && buf[off - 1] != '/') buf[off++] = '/';
                for (int i = 0; i < rlen; i++) buf[off + i] = buf[sizeof(buf) - 1 - rlen + i];
                buf[off + rlen] = 0;
            }
            goto restart;
        }

        cur = found;
    }
    return cur;
}

struct vfs_node *vfs_lookup(const char *path) {
    return vfs_lookup_impl(path, 0, 1, 0);
}

struct vfs_node *vfs_lookup_err(const char *path, int *err) {
    return vfs_lookup_impl(path, 0, 1, err);
}

struct vfs_node *vfs_lookup_nofollow(const char *path, int *err) {
    return vfs_lookup_impl(path, 0, 0, err);
}

struct vfs_node *lookup_parent(const char *path, const char **basename) {
    if (!path || path[0] != '/') return 0;

    int len = kstrlen(path);
    int last_slash = 0;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') { last_slash = i; break; }
    }

    *basename = path + last_slash + 1;
    if (!**basename) return 0;

    if (last_slash == 0) return vfs_root_node;

    char parent_path[256];
    int plen = last_slash;
    if (plen >= 256) plen = 255;
    for (int i = 0; i < plen; i++) parent_path[i] = path[i];
    parent_path[plen] = 0;

    return vfs_lookup(parent_path);
}

struct vfs_node *vfs_create(const char *path, int type) {
    const char *basename;
    struct vfs_node *parent = lookup_parent(path, &basename);
    if (!parent || parent->type != VFS_DIR) return 0;

    struct vfs_node *existing = vfs_lookup(path);
    if (existing) return existing;

    struct vfs_node *n = node_alloc(basename, type);
    if (!n) return 0;

    n->parent = parent;
    n->next = parent->children;
    parent->children = n;
    return n;
}

struct vfs_node *ensure_dirs(const char *path) {
    struct vfs_node *cur = vfs_root_node;
    const char *p = path;
    if (*p == '/') p++;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        const char *start = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - start);

        if (!*p) break;

        struct vfs_node *child = cur->children;
        struct vfs_node *found = 0;
        while (child) {
            int nlen = kstrlen(child->name);
            if (nlen == len) {
                int match = 1;
                for (int i = 0; i < len; i++) {
                    if (child->name[i] != start[i]) { match = 0; break; }
                }
                if (match) { found = child; break; }
            }
            child = child->next;
        }

        if (found) {
            cur = found;
        } else {
            char name[256];
            int copylen = len < 255 ? len : 255;
            for (int i = 0; i < copylen; i++) name[i] = start[i];
            name[copylen] = 0;

            struct vfs_node *n = node_alloc(name, VFS_DIR);
            if (!n) return 0;
            n->parent = cur;
            n->next = cur->children;
            cur->children = n;
            cur = n;
        }
    }
    return cur;
}
