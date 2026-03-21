/* mkfs.cosmo — Create a CosmoFS disk image
 *
 * Host tool (compiled with host gcc, not kernel gcc).
 * Usage: ./tools/mkfs disk.img SIZE_MB
 *
 * On-disk layout:
 *   Block 0:     Superblock
 *   Block 1..B:  Free-block bitmap
 *   Block B+1..I: Inode table
 *   Block I+1..J: Journal
 *   Block J+1..:  Data blocks
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define BLOCK_SIZE     4096
#define INODE_SIZE     256
#define INODES_PER_BLK (BLOCK_SIZE / INODE_SIZE)

/* Must match kernel/cosmofs.h */
#define COSMOFS_MAGIC_U64 0x0053466F6D736F43ULL
#define COSMOFS_VERSION   1

#define COSMOFS_TYPE_DIR  2

/* Journal size: 64 blocks = 256KB */
#define JOURNAL_BLOCKS 64

/* Inode table: enough for 1024 inodes = 64 blocks */
#define INODE_COUNT  1024
#define INODE_BLOCKS (INODE_COUNT / INODES_PER_BLK)

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
    uint64_t direct[12];
    uint64_t indirect;
    uint64_t double_indirect;
    uint64_t triple_indirect;
    uint64_t attr_block;
    uint64_t stream_block;
    uint64_t reserved[8];
} __attribute__((packed));

static void write_block(FILE *f, uint64_t block, const void *data) {
    fseek(f, (long)(block * BLOCK_SIZE), SEEK_SET);
    fwrite(data, BLOCK_SIZE, 1, f);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <disk.img> <size_mb>\n", argv[0]);
        return 1;
    }

    const char *outpath = argv[1];
    int size_mb = atoi(argv[2]);
    if (size_mb < 1 || size_mb > 65536) {
        fprintf(stderr, "Size must be 1-65536 MB\n");
        return 1;
    }

    uint64_t total_blocks = (uint64_t)size_mb * 1024 * 1024 / BLOCK_SIZE;

    /* Calculate bitmap blocks needed.
     * Each bitmap block covers 4096*8 = 32768 blocks. */
    uint32_t bitmap_blocks = (uint32_t)((total_blocks + 32767) / 32768);
    if (bitmap_blocks == 0) bitmap_blocks = 1;

    /* Layout */
    uint64_t bitmap_start  = 1;                                       /* after superblock */
    uint64_t inode_start   = bitmap_start + bitmap_blocks;
    uint64_t journal_start = inode_start + INODE_BLOCKS;
    uint64_t data_start    = journal_start + JOURNAL_BLOCKS;

    if (data_start >= total_blocks) {
        fprintf(stderr, "Disk too small for metadata\n");
        return 1;
    }

    uint64_t free_blocks = total_blocks - data_start;

    printf("mkfs.cosmo: %s\n", outpath);
    printf("  size:     %d MB (%lu blocks)\n", size_mb, (unsigned long)total_blocks);
    printf("  bitmap:   blocks %lu..%lu (%u blocks)\n",
           (unsigned long)bitmap_start,
           (unsigned long)(bitmap_start + bitmap_blocks - 1),
           bitmap_blocks);
    printf("  inodes:   blocks %lu..%lu (%d inodes)\n",
           (unsigned long)inode_start,
           (unsigned long)(inode_start + INODE_BLOCKS - 1),
           INODE_COUNT);
    printf("  journal:  blocks %lu..%lu (%d blocks)\n",
           (unsigned long)journal_start,
           (unsigned long)(journal_start + JOURNAL_BLOCKS - 1),
           JOURNAL_BLOCKS);
    printf("  data:     blocks %lu..%lu (%lu free)\n",
           (unsigned long)data_start,
           (unsigned long)(total_blocks - 1),
           (unsigned long)free_blocks);

    /* Create file */
    FILE *f = fopen(outpath, "wb");
    if (!f) { perror("fopen"); return 1; }

    /* Zero the entire image */
    uint8_t zero[BLOCK_SIZE];
    memset(zero, 0, BLOCK_SIZE);
    for (uint64_t i = 0; i < total_blocks; i++)
        fwrite(zero, BLOCK_SIZE, 1, f);

    /* Write superblock */
    struct cosmofs_super sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic         = COSMOFS_MAGIC_U64;
    sb.version       = COSMOFS_VERSION;
    sb.block_size    = BLOCK_SIZE;
    sb.total_blocks  = total_blocks;
    sb.free_blocks   = free_blocks - 1;  /* root dir takes 1 data block for btree */
    sb.bitmap_start  = bitmap_start;
    sb.bitmap_blocks = bitmap_blocks;
    sb.inode_start   = inode_start;
    sb.inode_blocks  = INODE_BLOCKS;
    sb.inode_count   = INODE_COUNT;
    sb.root_inode    = 1;  /* inode 1 = root directory */
    sb.journal_start = journal_start;
    sb.journal_size  = JOURNAL_BLOCKS;
    sb.data_start    = data_start;

    uint8_t sb_block[BLOCK_SIZE];
    memset(sb_block, 0, BLOCK_SIZE);
    memcpy(sb_block, &sb, sizeof(sb));
    write_block(f, 0, sb_block);

    /* Write root inode (inode 1) */
    uint8_t inode_block[BLOCK_SIZE];
    memset(inode_block, 0, BLOCK_SIZE);
    struct cosmofs_inode *root_in = (struct cosmofs_inode *)inode_block;
    root_in->type = COSMOFS_TYPE_DIR;
    root_in->direct[0] = 0;  /* no btree root yet (empty dir) */
    write_block(f, inode_start, inode_block);

    /* Mark first data block as used in bitmap (for future btree root).
     * Actually, root dir starts empty — no block allocated yet.
     * Just leave bitmap all-zeros (all free). */

    fclose(f);
    printf("mkfs.cosmo: done (%d MB)\n", size_mb);
    return 0;
}
