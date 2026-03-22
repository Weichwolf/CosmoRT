/* cosmo_cp — Copy files into a CosmoFS disk image
 *
 * Host tool (compiled with host gcc).
 * Usage: ./tools/cosmo_cp disk.img <host-path> <image-path>
 *        ./tools/cosmo_cp disk.img --mkdir <image-path>
 *        ./tools/cosmo_cp disk.img --write-string <string> <image-path>
 *        ./tools/cosmo_cp disk.img --tree <host-dir> <image-dir>
 *
 * Implements CosmoFS on-disk format: superblock, bitmap, inodes, B+ tree dirs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#define BLK 4096
#define INODE_SIZE 256
#define INODES_PER_BLK (BLK / INODE_SIZE)
#define DIRECT_BLOCKS 12

#define COSMOFS_MAGIC_U64 0x0053466F6D736F43ULL
#define TYPE_FILE    1
#define TYPE_DIR     2
#define TYPE_SYMLINK 3

#define BTREE_LEAF     1
#define BTREE_INTERNAL 2
#define BTREE_MAX_KEY  255

#define NODE_TYPE_OFF  0
#define NODE_COUNT_OFF 1
#define NODE_DATA_OFF  4

/* ── On-disk structures ─────────────────────────────── */

struct cosmofs_super {
    uint64_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t bitmap_start;
    uint32_t bitmap_blocks;
    uint32_t _pad0;
    uint64_t inode_start;
    uint32_t inode_blocks;
    uint32_t _pad1;
    uint64_t inode_count;
    uint64_t root_inode;
    uint64_t journal_start;
    uint32_t journal_size;
    uint32_t _pad2;
    uint64_t data_start;
} __attribute__((packed));

struct cosmofs_inode {
    uint16_t type;
    uint16_t flags;
    uint32_t uid;
    uint32_t gid;
    uint32_t _pad0;
    uint64_t size;
    uint64_t blocks_used;
    uint64_t ctime;
    uint64_t mtime;
    uint64_t atime;
    uint64_t direct[DIRECT_BLOCKS];
    uint64_t indirect;
    uint64_t double_indirect;
    uint64_t triple_indirect;
    uint64_t attr_block;
    uint64_t stream_block;
    uint64_t reserved[8];
} __attribute__((packed));

/* ── Global state ───────────────────────────────────── */

static FILE *disk;
static struct cosmofs_super sb;

/* ── Block I/O ──────────────────────────────────────── */

static void read_block(uint64_t block, void *buf) {
    fseek(disk, (long)(block * BLK), SEEK_SET);
    if (fread(buf, BLK, 1, disk) != 1) {
        fprintf(stderr, "read_block %lu failed\n", (unsigned long)block);
        exit(1);
    }
}

static void write_block(uint64_t block, const void *buf) {
    fseek(disk, (long)(block * BLK), SEEK_SET);
    if (fwrite(buf, BLK, 1, disk) != 1) {
        fprintf(stderr, "write_block %lu failed\n", (unsigned long)block);
        exit(1);
    }
}

/* ── Superblock ─────────────────────────────────────── */

static void sb_read(void) {
    uint8_t buf[BLK];
    read_block(0, buf);
    memcpy(&sb, buf, sizeof(sb));
    if (sb.magic != COSMOFS_MAGIC_U64) {
        fprintf(stderr, "Not a CosmoFS image\n");
        exit(1);
    }
}

static void sb_write(void) {
    uint8_t buf[BLK];
    read_block(0, buf);
    memcpy(buf, &sb, sizeof(sb));
    write_block(0, buf);
}

/* ── Block allocation ───────────────────────────────── */

static uint64_t block_alloc(void) {
    if (sb.free_blocks == 0) {
        fprintf(stderr, "No free blocks\n");
        exit(1);
    }

    for (uint32_t bi = 0; bi < sb.bitmap_blocks; bi++) {
        uint8_t buf[BLK];
        read_block(sb.bitmap_start + bi, buf);
        uint64_t *bits = (uint64_t *)buf;

        for (int w = 0; w < 512; w++) {
            if (bits[w] != ~0ULL) {
                int bit = __builtin_ctzll(~bits[w]);
                bits[w] |= (1ULL << bit);

                uint64_t block_nr = sb.data_start +
                    (uint64_t)bi * (4096 * 8) + (uint64_t)w * 64 + (uint64_t)bit;

                if (block_nr >= sb.total_blocks) {
                    fprintf(stderr, "Block allocation beyond disk\n");
                    exit(1);
                }

                write_block(sb.bitmap_start + bi, buf);
                sb.free_blocks--;

                /* Zero the allocated block */
                uint8_t zero[BLK];
                memset(zero, 0, BLK);
                write_block(block_nr, zero);

                return block_nr;
            }
        }
    }

    fprintf(stderr, "Bitmap full\n");
    exit(1);
}

/* ── Inode operations ───────────────────────────────── */

static void inode_read(uint64_t ino, struct cosmofs_inode *out) {
    uint64_t block_idx = (ino - 1) / INODES_PER_BLK;
    int slot = (int)((ino - 1) % INODES_PER_BLK);

    uint8_t buf[BLK];
    read_block(sb.inode_start + block_idx, buf);
    memcpy(out, buf + slot * INODE_SIZE, INODE_SIZE);
}

static void inode_write(uint64_t ino, const struct cosmofs_inode *in) {
    uint64_t block_idx = (ino - 1) / INODES_PER_BLK;
    int slot = (int)((ino - 1) % INODES_PER_BLK);

    uint8_t buf[BLK];
    read_block(sb.inode_start + block_idx, buf);
    memcpy(buf + slot * INODE_SIZE, in, INODE_SIZE);
    write_block(sb.inode_start + block_idx, buf);
}

static uint64_t inode_alloc(uint16_t type) {
    for (uint64_t ino = 1; ino <= sb.inode_count; ino++) {
        uint64_t block_idx = (ino - 1) / INODES_PER_BLK;
        int slot = (int)((ino - 1) % INODES_PER_BLK);

        uint8_t buf[BLK];
        read_block(sb.inode_start + block_idx, buf);
        struct cosmofs_inode *ip = (struct cosmofs_inode *)(buf + slot * INODE_SIZE);

        if (ip->type == 0) {
            memset(ip, 0, INODE_SIZE);
            ip->type = type;
            write_block(sb.inode_start + block_idx, buf);
            return ino;
        }
    }

    fprintf(stderr, "No free inodes\n");
    exit(1);
}

/* ── File block resolution (same as kernel) ─────────── */

static uint64_t resolve_block(struct cosmofs_inode *ip, uint64_t file_block, int alloc) {
    if (file_block < DIRECT_BLOCKS) {
        if (ip->direct[file_block] == 0 && alloc) {
            ip->direct[file_block] = block_alloc();
            ip->blocks_used++;
        }
        return ip->direct[file_block];
    }

    file_block -= DIRECT_BLOCKS;

    if (file_block < 512) {
        if (ip->indirect == 0) {
            if (!alloc) return 0;
            ip->indirect = block_alloc();
            ip->blocks_used++;
        }
        uint8_t buf[BLK];
        read_block(ip->indirect, buf);
        uint64_t *ptrs = (uint64_t *)buf;
        if (ptrs[file_block] == 0 && alloc) {
            ptrs[file_block] = block_alloc();
            ip->blocks_used++;
            write_block(ip->indirect, buf);
        }
        return ptrs[file_block];
    }

    file_block -= 512;

    if (file_block < 512ULL * 512) {
        if (ip->double_indirect == 0) {
            if (!alloc) return 0;
            ip->double_indirect = block_alloc();
            ip->blocks_used++;
        }
        int idx1 = (int)(file_block / 512);
        int idx2 = (int)(file_block % 512);

        uint8_t buf1[BLK];
        read_block(ip->double_indirect, buf1);
        uint64_t *ptrs1 = (uint64_t *)buf1;
        if (ptrs1[idx1] == 0 && alloc) {
            ptrs1[idx1] = block_alloc();
            ip->blocks_used++;
            write_block(ip->double_indirect, buf1);
        }
        uint64_t ind2 = ptrs1[idx1];
        if (ind2 == 0) return 0;

        uint8_t buf2[BLK];
        read_block(ind2, buf2);
        uint64_t *ptrs2 = (uint64_t *)buf2;
        if (ptrs2[idx2] == 0 && alloc) {
            ptrs2[idx2] = block_alloc();
            ip->blocks_used++;
            write_block(ind2, buf2);
        }
        return ptrs2[idx2];
    }

    fprintf(stderr, "File too large for double indirect\n");
    exit(1);
}

/* ── File data write ────────────────────────────────── */

static void file_write_data(uint64_t ino, const void *data, size_t len) {
    struct cosmofs_inode in;
    inode_read(ino, &in);

    const uint8_t *src = (const uint8_t *)data;
    size_t pos = 0;

    while (pos < len) {
        uint64_t file_block = pos / BLK;
        int block_off = (int)(pos % BLK);
        int chunk = BLK - block_off;
        if ((size_t)chunk > len - pos) chunk = (int)(len - pos);

        uint64_t disk_block = resolve_block(&in, file_block, 1);

        uint8_t buf[BLK];
        read_block(disk_block, buf);
        memcpy(buf + block_off, src + pos, (size_t)chunk);
        write_block(disk_block, buf);

        pos += (size_t)chunk;
    }

    in.size = len;
    in.flags = 0755;  /* executable permission hint */
    inode_write(ino, &in);
}

/* ── B+ tree operations (host-side, matches kernel) ─── */

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

static int read_key(const uint8_t *data, int *off, char *out, int maxlen) {
    uint16_t klen = (uint16_t)(data[*off] | (data[*off + 1] << 8));
    *off += 2;
    int cplen = (int)klen < maxlen - 1 ? (int)klen : maxlen - 1;
    for (int i = 0; i < cplen; i++) out[i] = (char)data[*off + i];
    out[cplen] = 0;
    *off += klen;
    return (int)klen;
}

static void write_key(uint8_t *data, int *off, const char *key, int klen) {
    data[*off]     = (uint8_t)(klen & 0xFF);
    data[*off + 1] = (uint8_t)((klen >> 8) & 0xFF);
    *off += 2;
    for (int i = 0; i < klen; i++) data[*off + i] = (uint8_t)key[i];
    *off += klen;
}

static int key_offset(const uint8_t *data, int idx) {
    int off = NODE_DATA_OFF;
    for (int i = 0; i < idx; i++) {
        uint16_t klen = (uint16_t)(data[off] | (data[off + 1] << 8));
        off += 2 + klen;
    }
    return off;
}

static int keys_section_size(const uint8_t *data, int count) {
    int off = NODE_DATA_OFF;
    for (int i = 0; i < count; i++) {
        uint16_t klen = (uint16_t)(data[off] | (data[off + 1] << 8));
        off += 2 + klen;
    }
    return off - NODE_DATA_OFF;
}

static uint64_t leaf_value(const uint8_t *data, int count, int idx) {
    int off = BLK - (count - idx) * 8;
    uint64_t v;
    memcpy(&v, data + off, 8);
    return v;
}

static void leaf_set_value(uint8_t *data, int count, int idx, uint64_t val) {
    int off = BLK - (count - idx) * 8;
    memcpy(data + off, &val, 8);
}

static uint64_t node_child(const uint8_t *data, int count, int idx) {
    int off = BLK - (count + 1 - idx) * 8;
    uint64_t v;
    memcpy(&v, data + off, 8);
    return v;
}

static void node_set_child(uint8_t *data, int count, int idx, uint64_t val) {
    int off = BLK - (count + 1 - idx) * 8;
    memcpy(data + off, &val, 8);
}

static int node_has_space(const uint8_t *data, int count, int type, int new_klen) {
    int keys_end = NODE_DATA_OFF + keys_section_size(data, count);
    int tail_size;
    if (type == BTREE_LEAF)
        tail_size = (count + 1) * 8;
    else
        tail_size = (count + 2) * 8;
    int needed = keys_end + (2 + new_klen) + tail_size;
    return needed <= BLK;
}

/* B+ tree search */
static int btree_search(uint64_t root_block, const char *key, uint64_t *value) {
    if (root_block == 0) return -1;

    int klen = (int)strlen(key);
    uint64_t cur = root_block;

    for (;;) {
        uint8_t data[BLK];
        read_block(cur, data);

        int type  = data[NODE_TYPE_OFF];
        int count = (int)(data[NODE_COUNT_OFF] | (data[NODE_COUNT_OFF + 1] << 8));

        if (type == BTREE_LEAF) {
            int off = NODE_DATA_OFF;
            for (int i = 0; i < count; i++) {
                char kbuf[BTREE_MAX_KEY + 1];
                int kl = read_key(data, &off, kbuf, sizeof(kbuf));
                if (keycmp(key, klen, kbuf, kl) == 0) {
                    *value = leaf_value(data, count, i);
                    return 0;
                }
                if (keycmp(key, klen, kbuf, kl) < 0) break;
            }
            return -1;
        }

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

        cur = node_child(data, count, child_idx);
    }
}

/* B+ tree insert — leaf */
struct split_result {
    int split;
    char median_key[BTREE_MAX_KEY + 1];
    int median_klen;
    uint64_t new_block;
};

#define MAX_ENTRIES 400

static int leaf_insert(uint64_t block, const char *key, int klen,
                       uint64_t value, struct split_result *sr) {
    sr->split = 0;

    uint8_t data[BLK];
    read_block(block, data);
    int count = (int)(data[NODE_COUNT_OFF] | (data[NODE_COUNT_OFF + 1] << 8));

    int pos = count;
    int off = NODE_DATA_OFF;
    for (int i = 0; i < count; i++) {
        char kbuf[BTREE_MAX_KEY + 1];
        int kl = read_key(data, &off, kbuf, sizeof(kbuf));
        int cmp = keycmp(key, klen, kbuf, kl);
        if (cmp == 0) {
            leaf_set_value(data, count, i, value);
            write_block(block, data);
            return 0;
        }
        if (cmp < 0) { pos = i; break; }
    }

    if (node_has_space(data, count, BTREE_LEAF, klen)) {
        uint8_t tmp[BLK];
        memcpy(tmp, data, BLK);

        int new_count = count + 1;
        data[NODE_COUNT_OFF]     = (uint8_t)(new_count & 0xFF);
        data[NODE_COUNT_OFF + 1] = (uint8_t)((new_count >> 8) & 0xFF);

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

        write_block(block, data);
        return 0;
    }

    /* Split */
    uint64_t new_blk = block_alloc();
    int total = count + 1;

    char all_keys[MAX_ENTRIES][BTREE_MAX_KEY + 1];
    int  all_klens[MAX_ENTRIES];
    uint64_t all_vals[MAX_ENTRIES];

    int src = NODE_DATA_OFF;
    int ai = 0;
    for (int i = 0; i < count; i++) {
        if (ai == pos) {
            memcpy(all_keys[ai], key, (size_t)klen);
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
        memcpy(all_keys[ai], key, (size_t)klen);
        all_keys[ai][klen] = 0;
        all_klens[ai] = klen;
        all_vals[ai] = value;
        ai++;
    }

    int mid = total / 2;

    /* Left half */
    memset(data, 0, BLK);
    data[NODE_TYPE_OFF] = BTREE_LEAF;
    data[NODE_COUNT_OFF]     = (uint8_t)(mid & 0xFF);
    data[NODE_COUNT_OFF + 1] = (uint8_t)((mid >> 8) & 0xFF);
    int woff = NODE_DATA_OFF;
    for (int i = 0; i < mid; i++)
        write_key(data, &woff, all_keys[i], all_klens[i]);
    for (int i = 0; i < mid; i++)
        leaf_set_value(data, mid, i, all_vals[i]);
    write_block(block, data);

    /* Right half */
    int right_count = total - mid;
    uint8_t rdata[BLK];
    memset(rdata, 0, BLK);
    rdata[NODE_TYPE_OFF] = BTREE_LEAF;
    rdata[NODE_COUNT_OFF]     = (uint8_t)(right_count & 0xFF);
    rdata[NODE_COUNT_OFF + 1] = (uint8_t)((right_count >> 8) & 0xFF);
    woff = NODE_DATA_OFF;
    for (int i = 0; i < right_count; i++)
        write_key(rdata, &woff, all_keys[mid + i], all_klens[mid + i]);
    for (int i = 0; i < right_count; i++)
        leaf_set_value(rdata, right_count, i, all_vals[mid + i]);
    write_block(new_blk, rdata);

    sr->split = 1;
    sr->median_klen = all_klens[mid];
    memcpy(sr->median_key, all_keys[mid], (size_t)sr->median_klen);
    sr->median_key[sr->median_klen] = 0;
    sr->new_block = new_blk;

    return 0;
}

static int do_insert(uint64_t block, const char *key, int klen,
                     uint64_t value, struct split_result *sr) {
    sr->split = 0;

    uint8_t data[BLK];
    read_block(block, data);
    int type  = data[NODE_TYPE_OFF];
    int count = (int)(data[NODE_COUNT_OFF] | (data[NODE_COUNT_OFF + 1] << 8));

    if (type == BTREE_LEAF)
        return leaf_insert(block, key, klen, value, sr);

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

    struct split_result child_sr;
    int rc = do_insert(child, key, klen, value, &child_sr);
    if (rc < 0) return rc;
    if (!child_sr.split) return 0;

    /* Re-read since child insert may have changed this block */
    read_block(block, data);
    count = (int)(data[NODE_COUNT_OFF] | (data[NODE_COUNT_OFF + 1] << 8));
    int ins_pos = child_idx;

    if (node_has_space(data, count, BTREE_INTERNAL, child_sr.median_klen)) {
        uint8_t tmp[BLK];
        memcpy(tmp, data, BLK);

        int new_count = count + 1;
        data[NODE_COUNT_OFF]     = (uint8_t)(new_count & 0xFF);
        data[NODE_COUNT_OFF + 1] = (uint8_t)((new_count >> 8) & 0xFF);

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

        for (int i = 0; i <= new_count; i++) {
            uint64_t c;
            if (i <= ins_pos)
                c = node_child(tmp, count, i < count + 1 ? i : count);
            else if (i == ins_pos + 1)
                c = child_sr.new_block;
            else
                c = node_child(tmp, count, i - 1);
            node_set_child(data, new_count, i, c);
        }

        write_block(block, data);
        return 0;
    }

    /* Internal node split */
    char int_keys[MAX_ENTRIES][BTREE_MAX_KEY + 1];
    int  int_klens[MAX_ENTRIES];
    uint64_t int_children[MAX_ENTRIES + 1];

    int total = count + 1;
    int soff2 = NODE_DATA_OFF;
    int ai = 0;
    for (int i = 0; i < count; i++) {
        if (ai == ins_pos) {
            memcpy(int_keys[ai], child_sr.median_key, (size_t)child_sr.median_klen);
            int_keys[ai][child_sr.median_klen] = 0;
            int_klens[ai] = child_sr.median_klen;
            ai++;
        }
        int_klens[ai] = read_key(data, &soff2, int_keys[ai], BTREE_MAX_KEY + 1);
        ai++;
    }
    if (ai == ins_pos) {
        memcpy(int_keys[ai], child_sr.median_key, (size_t)child_sr.median_klen);
        int_keys[ai][child_sr.median_klen] = 0;
        int_klens[ai] = child_sr.median_klen;
        ai++;
    }

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

    /* Left half */
    memset(data, 0, BLK);
    data[NODE_TYPE_OFF] = BTREE_INTERNAL;
    data[NODE_COUNT_OFF]     = (uint8_t)(mid & 0xFF);
    data[NODE_COUNT_OFF + 1] = (uint8_t)((mid >> 8) & 0xFF);
    int woff = NODE_DATA_OFF;
    for (int i = 0; i < mid; i++)
        write_key(data, &woff, int_keys[i], int_klens[i]);
    for (int i = 0; i <= mid; i++)
        node_set_child(data, mid, i, int_children[i]);
    write_block(block, data);

    /* Right half */
    uint64_t new_blk = block_alloc();
    int right_count = total - mid - 1;
    uint8_t rdata[BLK];
    memset(rdata, 0, BLK);
    rdata[NODE_TYPE_OFF] = BTREE_INTERNAL;
    rdata[NODE_COUNT_OFF]     = (uint8_t)(right_count & 0xFF);
    rdata[NODE_COUNT_OFF + 1] = (uint8_t)((right_count >> 8) & 0xFF);
    woff = NODE_DATA_OFF;
    for (int i = 0; i < right_count; i++)
        write_key(rdata, &woff, int_keys[mid + 1 + i], int_klens[mid + 1 + i]);
    for (int i = 0; i <= right_count; i++)
        node_set_child(rdata, right_count, i, int_children[mid + 1 + i]);
    write_block(new_blk, rdata);

    sr->split = 1;
    sr->median_klen = int_klens[mid];
    memcpy(sr->median_key, int_keys[mid], (size_t)sr->median_klen);
    sr->median_key[sr->median_klen] = 0;
    sr->new_block = new_blk;

    return 0;
}

static int btree_insert(uint64_t *root_block, const char *key, uint64_t value) {
    int klen = (int)strlen(key);

    if (*root_block == 0) {
        uint64_t blk = block_alloc();
        uint8_t data[BLK];
        memset(data, 0, BLK);
        data[NODE_TYPE_OFF] = BTREE_LEAF;
        data[NODE_COUNT_OFF] = 1;
        data[NODE_COUNT_OFF + 1] = 0;
        int off = NODE_DATA_OFF;
        write_key(data, &off, key, klen);
        leaf_set_value(data, 1, 0, value);
        write_block(blk, data);
        *root_block = blk;
        return 0;
    }

    struct split_result sr;
    int rc = do_insert(*root_block, key, klen, value, &sr);
    if (rc < 0) return rc;

    if (sr.split) {
        uint64_t new_root = block_alloc();
        uint8_t data[BLK];
        memset(data, 0, BLK);
        data[NODE_TYPE_OFF] = BTREE_INTERNAL;
        data[NODE_COUNT_OFF] = 1;
        data[NODE_COUNT_OFF + 1] = 0;
        int off = NODE_DATA_OFF;
        write_key(data, &off, sr.median_key, sr.median_klen);
        node_set_child(data, 1, 0, *root_block);
        node_set_child(data, 1, 1, sr.new_block);
        write_block(new_root, data);
        *root_block = new_root;
    }

    return 0;
}

/* ── Directory operations ───────────────────────────── */

static int dir_lookup(uint64_t dir_ino, const char *name, uint64_t *child_ino) {
    struct cosmofs_inode in;
    inode_read(dir_ino, &in);
    if (in.type != TYPE_DIR) return -1;
    if (in.direct[0] == 0) return -1;
    return btree_search(in.direct[0], name, child_ino);
}

static int dir_add_entry(uint64_t dir_ino, const char *name, uint64_t child_ino) {
    struct cosmofs_inode in;
    inode_read(dir_ino, &in);
    if (in.type != TYPE_DIR) return -1;

    uint64_t root_block = in.direct[0];
    int rc = btree_insert(&root_block, name, child_ino);
    if (rc < 0) return rc;

    in.direct[0] = root_block;
    in.size++;
    inode_write(dir_ino, &in);
    return 0;
}

/* Ensure a directory path exists, creating intermediates.
 * Returns inode number of the deepest directory. */
static uint64_t ensure_dir_path(const char *path) {
    uint64_t cur_ino = sb.root_inode;

    /* Skip leading / */
    const char *p = path;
    while (*p == '/') p++;

    while (*p) {
        const char *slash = p;
        while (*slash && *slash != '/') slash++;
        int namelen = (int)(slash - p);
        if (namelen == 0) { p = slash + 1; continue; }

        char name[256];
        if (namelen > 255) namelen = 255;
        memcpy(name, p, (size_t)namelen);
        name[namelen] = 0;

        uint64_t child_ino;
        if (dir_lookup(cur_ino, name, &child_ino) == 0) {
            cur_ino = child_ino;
        } else {
            /* Create directory */
            child_ino = inode_alloc(TYPE_DIR);
            dir_add_entry(cur_ino, name, child_ino);
            cur_ino = child_ino;
        }

        p = slash;
        while (*p == '/') p++;
    }

    return cur_ino;
}

/* ── Copy a single file ─────────────────────────────── */

static void copy_file(const char *host_path, const char *image_path) {
    /* Read host file */
    FILE *f = fopen(host_path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s: %s\n", host_path, strerror(errno));
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = malloc((size_t)fsize);
    if (!data) {
        fprintf(stderr, "malloc(%ld) failed\n", fsize);
        exit(1);
    }
    if (fsize > 0 && fread(data, (size_t)fsize, 1, f) != 1) {
        fprintf(stderr, "fread %s failed\n", host_path);
        exit(1);
    }
    fclose(f);

    /* Parse path: ensure parent directory exists */
    char parent[4096];
    char name[256];
    const char *last_slash = strrchr(image_path, '/');
    if (last_slash) {
        int plen = (int)(last_slash - image_path);
        if (plen == 0) plen = 1;
        memcpy(parent, image_path, (size_t)plen);
        parent[plen] = 0;
        strncpy(name, last_slash + 1, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
    } else {
        strcpy(parent, "/");
        strncpy(name, image_path, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
    }

    uint64_t parent_ino = ensure_dir_path(parent);

    /* Check if file already exists */
    uint64_t existing;
    if (dir_lookup(parent_ino, name, &existing) == 0) {
        /* Overwrite — just write data to existing inode */
        file_write_data(existing, data, (size_t)fsize);
    } else {
        /* Create new file */
        uint64_t file_ino = inode_alloc(TYPE_FILE);
        file_write_data(file_ino, data, (size_t)fsize);
        dir_add_entry(parent_ino, name, file_ino);
    }

    free(data);
    printf("  %s → %s (%ld bytes)\n", host_path, image_path, fsize);
}

/* ── Copy a directory tree recursively ──────────────── */

static void copy_tree(const char *host_dir, const char *image_dir) {
    DIR *d = opendir(host_dir);
    if (!d) {
        fprintf(stderr, "Cannot open dir %s: %s\n", host_dir, strerror(errno));
        return;
    }

    ensure_dir_path(image_dir);

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char host_path[4096];
        char img_path[4096];
        snprintf(host_path, sizeof(host_path), "%s/%s", host_dir, ent->d_name);
        snprintf(img_path, sizeof(img_path), "%s/%s", image_dir, ent->d_name);

        struct stat st;
        if (lstat(host_path, &st) < 0) continue;

        if (S_ISDIR(st.st_mode)) {
            copy_tree(host_path, img_path);
        } else if (S_ISREG(st.st_mode)) {
            copy_file(host_path, img_path);
        } else if (S_ISLNK(st.st_mode)) {
            /* Create symlink: store target in file data */
            char target[4096];
            ssize_t tlen = readlink(host_path, target, sizeof(target) - 1);
            if (tlen < 0) continue;
            target[tlen] = 0;

            char parent[4096];
            snprintf(parent, sizeof(parent), "%s", image_dir);
            uint64_t parent_ino = ensure_dir_path(parent);

            uint64_t sym_ino = inode_alloc(TYPE_SYMLINK);
            file_write_data(sym_ino, target, (size_t)tlen);
            dir_add_entry(parent_ino, ent->d_name, sym_ino);
            printf("  %s → %s (symlink → %s)\n", host_path, img_path, target);
        }
    }

    closedir(d);
}

/* ── Write a string to a file ───────────────────────── */

static void write_string(const char *str, const char *image_path) {
    size_t len = strlen(str);

    char parent[4096];
    char name[256];
    const char *last_slash = strrchr(image_path, '/');
    if (last_slash) {
        int plen = (int)(last_slash - image_path);
        if (plen == 0) plen = 1;
        memcpy(parent, image_path, (size_t)plen);
        parent[plen] = 0;
        strncpy(name, last_slash + 1, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
    } else {
        strcpy(parent, "/");
        strncpy(name, image_path, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
    }

    uint64_t parent_ino = ensure_dir_path(parent);
    uint64_t file_ino = inode_alloc(TYPE_FILE);
    file_write_data(file_ino, str, len);
    dir_add_entry(parent_ino, name, file_ino);
    printf("  (string) → %s (%zu bytes)\n", image_path, len);
}

/* ── Main ───────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s disk.img <host-path> <image-path>\n", argv[0]);
        fprintf(stderr, "  %s disk.img --mkdir <image-path>\n", argv[0]);
        fprintf(stderr, "  %s disk.img --write-string <string> <image-path>\n", argv[0]);
        fprintf(stderr, "  %s disk.img --tree <host-dir> <image-dir>\n", argv[0]);
        return 1;
    }

    const char *img_path = argv[1];
    disk = fopen(img_path, "r+b");
    if (!disk) {
        fprintf(stderr, "Cannot open %s: %s\n", img_path, strerror(errno));
        return 1;
    }

    sb_read();

    if (strcmp(argv[2], "--mkdir") == 0) {
        if (argc < 4) { fprintf(stderr, "Missing path\n"); return 1; }
        ensure_dir_path(argv[3]);
        printf("  mkdir %s\n", argv[3]);
    } else if (strcmp(argv[2], "--write-string") == 0) {
        if (argc < 5) { fprintf(stderr, "Missing string or path\n"); return 1; }
        write_string(argv[3], argv[4]);
    } else if (strcmp(argv[2], "--tree") == 0) {
        if (argc < 5) { fprintf(stderr, "Missing host-dir or image-dir\n"); return 1; }
        copy_tree(argv[3], argv[4]);
    } else {
        /* Default: copy single file */
        if (argc < 4) { fprintf(stderr, "Missing paths\n"); return 1; }
        copy_file(argv[2], argv[3]);
    }

    sb_write();
    fclose(disk);
    return 0;
}
