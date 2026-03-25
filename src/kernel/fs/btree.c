/* CosmoRT B+ Tree — persistent on-disk B+ tree
 *
 * All nodes are 4KB blocks. Keys are variable-length strings.
 * Values are uint64_t. Accesses disk only through bcache.
 *
 * Simplified design: no borrowing on delete (just merge).
 * Order adapts to key sizes. Worst case ~20 keys per node
 * (255-byte keys), typical case ~100+ (short filenames).
 */

#include "fs/btree.h"
#include "fs/bcache.h"
#include "memops.h"
#include "hw/serial.h"
#include "sys/syscall.h"  /* ENOENT, ENOMEM */

/* Block allocator — provided by cosmofs */
extern uint64_t cosmofs_block_alloc(void);
extern void cosmofs_block_free(uint64_t block);

/* ── Node format ──────────────────────────────────── */

/* Header at offset 0 */
#define NODE_TYPE_OFF    0
#define NODE_COUNT_OFF   1
#define NODE_DATA_OFF    4  /* start of keys/values/children */

#define BLK 4096

/* Key comparison */
static int keycmp(const char *a, int alen, const char *b, int blen) {
    int minlen = alen < blen ? alen : blen;
    for (int i = 0; i < minlen; i++) {
        if ((unsigned char)a[i] < (unsigned char)b[i]) return -1;
        if ((unsigned char)a[i] > (unsigned char)b[i]) return  1;
    }
    if (alen < blen) return -1;
    if (alen > blen) return  1;
    return 0;
}

static int kstrlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* ── Node read helpers ────────────────────────────── */

/* Read key at position idx from node data.
 * Returns key length, copies key to out (null-terminated).
 * *off is updated to point past this key. */
static int read_key(const uint8_t *data, int *off, char *out, int maxlen) {
    uint16_t klen = (uint16_t)(data[*off] | (data[*off + 1] << 8));
    *off += 2;
    int cplen = (int)klen < maxlen - 1 ? (int)klen : maxlen - 1;
    for (int i = 0; i < cplen; i++) out[i] = (char)data[*off + i];
    out[cplen] = 0;
    *off += klen;
    return (int)klen;
}

/* Write key at *off in data. Returns bytes written. */
static void write_key(uint8_t *data, int *off, const char *key, int klen) {
    data[*off]     = (uint8_t)(klen & 0xFF);
    data[*off + 1] = (uint8_t)((klen >> 8) & 0xFF);
    *off += 2;
    for (int i = 0; i < klen; i++) data[*off + i] = (uint8_t)key[i];
    *off += klen;
}

/* Calculate total key section size for count keys */
static int keys_section_size(const uint8_t *data, int count) {
    int off = NODE_DATA_OFF;
    for (int i = 0; i < count; i++) {
        uint16_t klen = (uint16_t)(data[off] | (data[off + 1] << 8));
        off += 2 + klen;
    }
    return off - NODE_DATA_OFF;
}

/* Find key offset and section end for key at index idx */
static int key_offset(const uint8_t *data, int idx) {
    int off = NODE_DATA_OFF;
    for (int i = 0; i < idx; i++) {
        uint16_t klen = (uint16_t)(data[off] | (data[off + 1] << 8));
        off += 2 + klen;
    }
    return off;
}

/* ── Node layout for values/children ──────────────── */

/* For leaf nodes: values are packed after all keys.
 * For internal nodes: children (count+1 × uint64_t) are packed after all keys.
 *
 * Layout:
 *   [header 4B][keys...][values or children at end of block]
 *
 * Values/children are stored at the END of the block, growing backwards.
 * This avoids having to know total key section size upfront.
 *
 * Actually, simpler approach: values/children stored at fixed offset
 * from end of block.
 *
 * Leaf:     values[i] at BLK - (count - i) * 8
 * Internal: children[i] at BLK - (count + 1 - i) * 8
 */

static uint64_t leaf_value(const uint8_t *data, int count, int idx) {
    int off = BLK - (count - idx) * 8;
    uint64_t v;
    kmemcpy(&v, data + off, 8);
    return v;
}

static void leaf_set_value(uint8_t *data, int count, int idx, uint64_t val) {
    int off = BLK - (count - idx) * 8;
    kmemcpy(data + off, &val, 8);
}

static uint64_t node_child(const uint8_t *data, int count, int idx) {
    int off = BLK - (count + 1 - idx) * 8;
    uint64_t v;
    kmemcpy(&v, data + off, 8);
    return v;
}

static void node_set_child(uint8_t *data, int count, int idx, uint64_t val) {
    int off = BLK - (count + 1 - idx) * 8;
    kmemcpy(data + off, &val, 8);
}

/* Check if a new key of klen bytes fits in a node with current data */
static int node_has_space(const uint8_t *data, int count, int type, int new_klen) {
    int keys_end = NODE_DATA_OFF + keys_section_size(data, count);
    int tail_size;
    if (type == BTREE_LEAF)
        tail_size = (count + 1) * 8;  /* one more value */
    else
        tail_size = (count + 2) * 8;  /* one more child */
    int needed = keys_end + (2 + new_klen) + tail_size;
    return needed <= BLK;
}

/* ── Search ───────────────────────────────────────── */

int btree_search(uint64_t root_block, const char *key, uint64_t *value) {
    if (root_block == 0) return -ENOENT;

    int klen = kstrlen(key);
    uint64_t cur = root_block;

    for (;;) {
        struct bcache_entry *be = bcache_get(cur);
        if (!be) return -EIO;

        uint8_t *data = be->data;
        int type  = data[NODE_TYPE_OFF];
        int count = (int)(data[NODE_COUNT_OFF] | (data[NODE_COUNT_OFF + 1] << 8));

        if (type == BTREE_LEAF) {
            /* Linear search through keys */
            int off = NODE_DATA_OFF;
            for (int i = 0; i < count; i++) {
                char kbuf[BTREE_MAX_KEY + 1];
                int kl = read_key(data, &off, kbuf, sizeof(kbuf));
                int cmp = keycmp(key, klen, kbuf, kl);
                if (cmp == 0) {
                    *value = leaf_value(data, count, i);
                    bcache_put(be);
                    return 0;
                }
                if (cmp < 0) break;  /* keys are sorted */
            }
            bcache_put(be);
            return -ENOENT;
        }

        /* Internal node: find child */
        int off = NODE_DATA_OFF;
        int child_idx = count;  /* default: rightmost child */
        for (int i = 0; i < count; i++) {
            char kbuf[BTREE_MAX_KEY + 1];
            int kl = read_key(data, &off, kbuf, sizeof(kbuf));
            if (keycmp(key, klen, kbuf, kl) < 0) {
                child_idx = i;
                break;
            }
        }

        uint64_t next = node_child(data, count, child_idx);
        bcache_put(be);
        cur = next;
    }
}

/* ── Insert (with split) ──────────────────────────── */

/* Split result passed up during recursive insert */
struct split_result {
    int split;              /* 1 if split occurred */
    char median_key[BTREE_MAX_KEY + 1];
    int median_klen;
    uint64_t new_block;     /* right half after split */
};

/* Insert into a leaf node. Returns 0 on success, fills split_result if split needed. */
static int leaf_insert(uint64_t block, const char *key, int klen,
                       uint64_t value, struct split_result *sr) {
    sr->split = 0;

    struct bcache_entry *be = bcache_get(block);
    if (!be) return -EIO;

    uint8_t *data = be->data;
    int count = (int)(data[NODE_COUNT_OFF] | (data[NODE_COUNT_OFF + 1] << 8));

    /* Find insertion position */
    int pos = count;
    int off = NODE_DATA_OFF;
    for (int i = 0; i < count; i++) {
        char kbuf[BTREE_MAX_KEY + 1];
        int kl = read_key(data, &off, kbuf, sizeof(kbuf));
        int cmp = keycmp(key, klen, kbuf, kl);
        if (cmp == 0) {
            /* Key exists — update value */
            leaf_set_value(data, count, i, value);
            bcache_mark_dirty(be);
            bcache_put(be);
            return 0;
        }
        if (cmp < 0) { pos = i; break; }
    }

    /* Check if it fits */
    if (node_has_space(data, count, BTREE_LEAF, klen)) {
        /* Build new node in-place by rebuilding key+value arrays */
        uint8_t tmp[BLK];
        kmemcpy(tmp, data, BLK);

        /* Write header */
        int new_count = count + 1;
        data[NODE_COUNT_OFF]     = (uint8_t)(new_count & 0xFF);
        data[NODE_COUNT_OFF + 1] = (uint8_t)((new_count >> 8) & 0xFF);

        /* Write keys */
        int src_off = NODE_DATA_OFF;
        int dst_off = NODE_DATA_OFF;
        for (int i = 0; i < new_count; i++) {
            if (i == pos) {
                write_key(data, &dst_off, key, klen);
            } else {
                int src_i = (i < pos) ? i : i - 1;
                int soff = key_offset(tmp, src_i);
                uint16_t skl = (uint16_t)(tmp[soff] | (tmp[soff + 1] << 8));
                write_key(data, &dst_off, (char *)(tmp + soff + 2), (int)skl);
            }
        }

        /* Write values from end */
        for (int i = 0; i < new_count; i++) {
            uint64_t v;
            if (i == pos)
                v = value;
            else {
                int src_i = (i < pos) ? i : i - 1;
                v = leaf_value(tmp, count, src_i);
            }
            leaf_set_value(data, new_count, i, v);
        }

        bcache_mark_dirty(be);
        bcache_put(be);
        (void)src_off;
        return 0;
    }

    /* Node full — split */
    uint64_t new_blk = cosmofs_block_alloc();
    if (new_blk == 0) { bcache_put(be); return -ENOMEM; }

    /* Collect all keys+values including the new one */
    int total = count + 1;
    /* Use temporary arrays. Max entries per leaf with 1-byte keys:
     * (4096 - 4) / (2 + 1 + 8) ≈ 372. Conservative limit. */
    #define MAX_LEAF_ENTRIES 400
    char all_keys[MAX_LEAF_ENTRIES][BTREE_MAX_KEY + 1];
    int  all_klens[MAX_LEAF_ENTRIES];
    uint64_t all_vals[MAX_LEAF_ENTRIES];

    int src = NODE_DATA_OFF;
    int ai = 0;
    for (int i = 0; i < count; i++) {
        if (ai == pos) {
            /* Insert new key here */
            for (int j = 0; j < klen; j++) all_keys[ai][j] = key[j];
            all_keys[ai][klen] = 0;
            all_klens[ai] = klen;
            all_vals[ai] = value;
            ai++;
        }
        all_klens[ai] = read_key(data, &src, all_keys[ai], BTREE_MAX_KEY + 1);
        all_vals[ai] = leaf_value(data, count, i);
        ai++;
    }
    if (ai == pos) {
        for (int j = 0; j < klen; j++) all_keys[ai][j] = key[j];
        all_keys[ai][klen] = 0;
        all_klens[ai] = klen;
        all_vals[ai] = value;
        ai++;
    }

    int mid = total / 2;

    /* Left half stays in current block */
    kmemset(data, 0, BLK);
    data[NODE_TYPE_OFF] = BTREE_LEAF;
    data[NODE_COUNT_OFF]     = (uint8_t)(mid & 0xFF);
    data[NODE_COUNT_OFF + 1] = (uint8_t)((mid >> 8) & 0xFF);
    int woff = NODE_DATA_OFF;
    for (int i = 0; i < mid; i++)
        write_key(data, &woff, all_keys[i], all_klens[i]);
    for (int i = 0; i < mid; i++)
        leaf_set_value(data, mid, i, all_vals[i]);
    bcache_mark_dirty(be);
    bcache_put(be);

    /* Right half in new block */
    int right_count = total - mid;
    struct bcache_entry *rbe = bcache_get(new_blk);
    if (!rbe) return -EIO;
    uint8_t *rdata = rbe->data;
    kmemset(rdata, 0, BLK);
    rdata[NODE_TYPE_OFF] = BTREE_LEAF;
    rdata[NODE_COUNT_OFF]     = (uint8_t)(right_count & 0xFF);
    rdata[NODE_COUNT_OFF + 1] = (uint8_t)((right_count >> 8) & 0xFF);
    woff = NODE_DATA_OFF;
    for (int i = 0; i < right_count; i++)
        write_key(rdata, &woff, all_keys[mid + i], all_klens[mid + i]);
    for (int i = 0; i < right_count; i++)
        leaf_set_value(rdata, right_count, i, all_vals[mid + i]);
    bcache_mark_dirty(rbe);
    bcache_put(rbe);

    /* Median key goes up */
    sr->split = 1;
    sr->median_klen = all_klens[mid];
    for (int i = 0; i < sr->median_klen; i++)
        sr->median_key[i] = all_keys[mid][i];
    sr->median_key[sr->median_klen] = 0;
    sr->new_block = new_blk;

    return 0;
}

/* Recursive insert into a subtree rooted at 'block'. */
static int do_insert(uint64_t block, const char *key, int klen,
                     uint64_t value, struct split_result *sr) {
    sr->split = 0;

    struct bcache_entry *be = bcache_get(block);
    if (!be) return -EIO;

    uint8_t *data = be->data;
    int type  = data[NODE_TYPE_OFF];
    int count = (int)(data[NODE_COUNT_OFF] | (data[NODE_COUNT_OFF + 1] << 8));

    if (type == BTREE_LEAF) {
        bcache_put(be);
        return leaf_insert(block, key, klen, value, sr);
    }

    /* Internal node: find child to recurse into */
    int off = NODE_DATA_OFF;
    int child_idx = count;
    for (int i = 0; i < count; i++) {
        char kbuf[BTREE_MAX_KEY + 1];
        int kl = read_key(data, &off, kbuf, sizeof(kbuf));
        if (keycmp(key, klen, kbuf, kl) < 0) {
            child_idx = i;
            break;
        }
    }

    uint64_t child = node_child(data, count, child_idx);
    bcache_put(be);

    /* Recurse */
    struct split_result child_sr;
    int rc = do_insert(child, key, klen, value, &child_sr);
    if (rc < 0) return rc;

    if (!child_sr.split) return 0;

    /* Child split — insert median key + new child pointer into this node */
    be = bcache_get(block);
    if (!be) return -EIO;
    data = be->data;
    count = (int)(data[NODE_COUNT_OFF] | (data[NODE_COUNT_OFF + 1] << 8));

    int ins_pos = child_idx;  /* insert before the key at child_idx position */

    if (node_has_space(data, count, BTREE_INTERNAL, child_sr.median_klen)) {
        /* Fits — rebuild node */
        uint8_t tmp[BLK];
        kmemcpy(tmp, data, BLK);

        int new_count = count + 1;
        data[NODE_COUNT_OFF]     = (uint8_t)(new_count & 0xFF);
        data[NODE_COUNT_OFF + 1] = (uint8_t)((new_count >> 8) & 0xFF);

        /* Rebuild keys */
        int dst_off = NODE_DATA_OFF;
        for (int i = 0; i < new_count; i++) {
            if (i == ins_pos) {
                write_key(data, &dst_off, child_sr.median_key, child_sr.median_klen);
            } else {
                int si = (i < ins_pos) ? i : i - 1;
                int soff = key_offset(tmp, si);
                uint16_t skl = (uint16_t)(tmp[soff] | (tmp[soff + 1] << 8));
                write_key(data, &dst_off, (char *)(tmp + soff + 2), (int)skl);
            }
        }

        /* Rebuild children: new_count + 1 children */
        for (int i = 0; i <= new_count; i++) {
            uint64_t c;
            if (i <= ins_pos) {
                c = node_child(tmp, count, i < count + 1 ? i : count);
            } else if (i == ins_pos + 1) {
                c = child_sr.new_block;
            } else {
                c = node_child(tmp, count, i - 1);
            }
            node_set_child(data, new_count, i, c);
        }

        bcache_mark_dirty(be);
        bcache_put(be);
        return 0;
    }

    /* Internal node full — split */
    #define MAX_INTERNAL_ENTRIES 400
    char int_keys[MAX_INTERNAL_ENTRIES][BTREE_MAX_KEY + 1];
    int  int_klens[MAX_INTERNAL_ENTRIES];
    uint64_t int_children[MAX_INTERNAL_ENTRIES + 1];

    int total = count + 1;
    int soff = NODE_DATA_OFF;
    int ai = 0;
    for (int i = 0; i < count; i++) {
        if (ai == ins_pos) {
            for (int j = 0; j < child_sr.median_klen; j++)
                int_keys[ai][j] = child_sr.median_key[j];
            int_keys[ai][child_sr.median_klen] = 0;
            int_klens[ai] = child_sr.median_klen;
            ai++;
        }
        int_klens[ai] = read_key(data, &soff, int_keys[ai], BTREE_MAX_KEY + 1);
        ai++;
    }
    if (ai == ins_pos) {
        for (int j = 0; j < child_sr.median_klen; j++)
            int_keys[ai][j] = child_sr.median_key[j];
        int_keys[ai][child_sr.median_klen] = 0;
        int_klens[ai] = child_sr.median_klen;
        ai++;
    }

    /* Build children array */
    ai = 0;
    for (int i = 0; i <= count; i++) {
        if (ai == ins_pos + 1) {
            int_children[ai] = child_sr.new_block;
            ai++;
        }
        int_children[ai] = node_child(data, count, i);
        ai++;
    }
    if (ai == ins_pos + 1) {
        int_children[ai] = child_sr.new_block;
        ai++;
    }

    int mid = total / 2;

    /* Left half: keys [0..mid-1], children [0..mid] */
    kmemset(data, 0, BLK);
    data[NODE_TYPE_OFF] = BTREE_INTERNAL;
    data[NODE_COUNT_OFF]     = (uint8_t)(mid & 0xFF);
    data[NODE_COUNT_OFF + 1] = (uint8_t)((mid >> 8) & 0xFF);
    int woff = NODE_DATA_OFF;
    for (int i = 0; i < mid; i++)
        write_key(data, &woff, int_keys[i], int_klens[i]);
    for (int i = 0; i <= mid; i++)
        node_set_child(data, mid, i, int_children[i]);
    bcache_mark_dirty(be);
    bcache_put(be);

    /* Right half: keys [mid+1..total-1], children [mid+1..total] */
    uint64_t new_blk = cosmofs_block_alloc();
    if (new_blk == 0) return -ENOMEM;

    int right_count = total - mid - 1;
    struct bcache_entry *rbe = bcache_get(new_blk);
    if (!rbe) return -EIO;
    uint8_t *rdata = rbe->data;
    kmemset(rdata, 0, BLK);
    rdata[NODE_TYPE_OFF] = BTREE_INTERNAL;
    rdata[NODE_COUNT_OFF]     = (uint8_t)(right_count & 0xFF);
    rdata[NODE_COUNT_OFF + 1] = (uint8_t)((right_count >> 8) & 0xFF);
    woff = NODE_DATA_OFF;
    for (int i = 0; i < right_count; i++)
        write_key(rdata, &woff, int_keys[mid + 1 + i], int_klens[mid + 1 + i]);
    for (int i = 0; i <= right_count; i++)
        node_set_child(rdata, right_count, i, int_children[mid + 1 + i]);
    bcache_mark_dirty(rbe);
    bcache_put(rbe);

    /* Median key [mid] goes up */
    sr->split = 1;
    sr->median_klen = int_klens[mid];
    for (int i = 0; i < sr->median_klen; i++)
        sr->median_key[i] = int_keys[mid][i];
    sr->median_key[sr->median_klen] = 0;
    sr->new_block = new_blk;

    return 0;
}

int btree_insert(uint64_t *root_block, const char *key, uint64_t value) {
    int klen = kstrlen(key);

    /* Empty tree — create leaf */
    if (*root_block == 0) {
        uint64_t blk = cosmofs_block_alloc();
        if (blk == 0) return -ENOMEM;

        struct bcache_entry *be = bcache_get(blk);
        if (!be) return -EIO;
        uint8_t *data = be->data;
        kmemset(data, 0, BLK);
        data[NODE_TYPE_OFF] = BTREE_LEAF;
        data[NODE_COUNT_OFF] = 1;
        data[NODE_COUNT_OFF + 1] = 0;
        int off = NODE_DATA_OFF;
        write_key(data, &off, key, klen);
        leaf_set_value(data, 1, 0, value);
        bcache_mark_dirty(be);
        bcache_put(be);
        *root_block = blk;
        return 0;
    }

    struct split_result sr;
    int rc = do_insert(*root_block, key, klen, value, &sr);
    if (rc < 0) return rc;

    if (sr.split) {
        /* Root split — create new root */
        uint64_t new_root = cosmofs_block_alloc();
        if (new_root == 0) return -ENOMEM;

        struct bcache_entry *be = bcache_get(new_root);
        if (!be) return -EIO;
        uint8_t *data = be->data;
        kmemset(data, 0, BLK);
        data[NODE_TYPE_OFF] = BTREE_INTERNAL;
        data[NODE_COUNT_OFF] = 1;
        data[NODE_COUNT_OFF + 1] = 0;
        int off = NODE_DATA_OFF;
        write_key(data, &off, sr.median_key, sr.median_klen);
        node_set_child(data, 1, 0, *root_block);
        node_set_child(data, 1, 1, sr.new_block);
        bcache_mark_dirty(be);
        bcache_put(be);
        *root_block = new_root;
    }

    return 0;
}

/* ── Delete ───────────────────────────────────────── */

/* Simple delete: find leaf, remove entry.
 * No rebalancing/merging for now — nodes may become underfull.
 * This is fine for a filesystem where deletes are less frequent than inserts. */

int btree_delete(uint64_t *root_block, const char *key) {
    if (*root_block == 0) return -ENOENT;

    int klen = kstrlen(key);
    uint64_t cur = *root_block;

    /* Walk to leaf, keeping path for potential cleanup */
    /* For simplicity: only handle leaf deletion without rebalancing */

    for (;;) {
        struct bcache_entry *be = bcache_get(cur);
        if (!be) return -EIO;

        uint8_t *data = be->data;
        int type  = data[NODE_TYPE_OFF];
        int count = (int)(data[NODE_COUNT_OFF] | (data[NODE_COUNT_OFF + 1] << 8));
        int off;

        if (type == BTREE_LEAF) {
            /* Find and remove key */
            off = NODE_DATA_OFF;
            int found = -1;
            for (int i = 0; i < count; i++) {
                char kbuf[BTREE_MAX_KEY + 1];
                int kl = read_key(data, &off, kbuf, sizeof(kbuf));
                if (keycmp(key, klen, kbuf, kl) == 0) {
                    found = i;
                    break;
                }
            }

            if (found < 0) {
                bcache_put(be);
                return -ENOENT;
            }

            /* Rebuild without the found key */
            uint8_t tmp[BLK];
            kmemcpy(tmp, data, BLK);

            int new_count = count - 1;

            if (new_count == 0) {
                /* Node empty — free block, set root to 0 */
                bcache_put(be);
                cosmofs_block_free(cur);
                if (cur == *root_block) *root_block = 0;
                return 0;
            }

            data[NODE_COUNT_OFF]     = (uint8_t)(new_count & 0xFF);
            data[NODE_COUNT_OFF + 1] = (uint8_t)((new_count >> 8) & 0xFF);

            int dst_off = NODE_DATA_OFF;
            int si = 0;
            for (int i = 0; i < count; i++) {
                if (i == found) continue;
                int soff = key_offset(tmp, i);
                uint16_t skl = (uint16_t)(tmp[soff] | (tmp[soff + 1] << 8));
                write_key(data, &dst_off, (char *)(tmp + soff + 2), (int)skl);
                uint64_t v = leaf_value(tmp, count, i);
                leaf_set_value(data, new_count, si, v);
                si++;
            }

            /* Clear remaining space */
            if (dst_off < BLK - new_count * 8)
                kmemset(data + dst_off, 0, (size_t)(BLK - new_count * 8 - dst_off));

            bcache_mark_dirty(be);
            bcache_put(be);
            return 0;
        }

        /* Internal node — find child */
        off = NODE_DATA_OFF;
        int child_idx = count;
        for (int i = 0; i < count; i++) {
            char kbuf[BTREE_MAX_KEY + 1];
            int kl = read_key(data, &off, kbuf, sizeof(kbuf));
            if (keycmp(key, klen, kbuf, kl) < 0) {
                child_idx = i;
                break;
            }
        }

        uint64_t next = node_child(data, count, child_idx);
        bcache_put(be);
        cur = next;
    }
}

/* ── Iterate ──────────────────────────────────────── */

static int do_iterate(uint64_t block, int *skip, int *visited,
                      int (*cb)(const char *, uint64_t, void *), void *ctx) {
    if (block == 0) return 0;

    struct bcache_entry *be = bcache_get(block);
    if (!be) return 0;

    uint8_t *data = be->data;
    int type  = data[NODE_TYPE_OFF];
    int count = (int)(data[NODE_COUNT_OFF] | (data[NODE_COUNT_OFF + 1] << 8));

    if (type == BTREE_LEAF) {
        int off = NODE_DATA_OFF;
        for (int i = 0; i < count; i++) {
            char kbuf[BTREE_MAX_KEY + 1];
            read_key(data, &off, kbuf, sizeof(kbuf));
            if (*skip > 0) {
                (*skip)--;
                continue;
            }
            uint64_t val = leaf_value(data, count, i);
            (*visited)++;
            if (cb(kbuf, val, ctx) != 0) {
                bcache_put(be);
                return 1;  /* stop */
            }
        }
        bcache_put(be);
        return 0;
    }

    /* Internal: recurse into children in order */
    /* Read all children first (since we need to release cache entry) */
    uint64_t children[MAX_INTERNAL_ENTRIES + 1];
    for (int i = 0; i <= count; i++)
        children[i] = node_child(data, count, i);
    bcache_put(be);

    for (int i = 0; i <= count; i++) {
        if (do_iterate(children[i], skip, visited, cb, ctx))
            return 1;
    }

    return 0;
}

int btree_iterate(uint64_t root_block, int offset,
                  int (*cb)(const char *key, uint64_t value, void *ctx), void *ctx) {
    int skip = offset;
    int visited = 0;
    do_iterate(root_block, &skip, &visited, cb, ctx);
    return visited;
}
