/* CosmoRT Journal — metadata-only write-ahead log (ring buffer on disk)
 *
 * Design:
 * - Journal occupies a contiguous range of blocks on disk.
 * - Transactions are written sequentially; ring wraps around.
 * - On commit: write all logged blocks to journal, write commit record,
 *   then write blocks to their final locations ("checkpoint").
 * - On recovery: scan journal for complete transactions and replay.
 *
 * Simplified: one transaction at a time (no concurrent tx).
 * Journal blocks are written directly via blk_write (bypassing bcache)
 * to avoid circular dependency. Final block writes go through bcache.
 */

#include "fs/journal.h"
#include "fs/bcache.h"
#include "memops.h"
#include "hw/serial.h"
#include "spinlock.h"

/* Direct block I/O for journal writes (avoids bcache for journal area) */
extern int blk_read(uint64_t block, void *buf);
extern int blk_write(uint64_t block, const void *buf);

/* ── Journal state ────────────────────────────────── */

static uint64_t jnl_start;      /* first journal block on disk */
static uint32_t jnl_size;       /* journal size in blocks */
static uint32_t jnl_head;       /* next write position (offset from start) */
static uint32_t jnl_tx_id;      /* monotonic transaction ID */
static int      jnl_in_tx;      /* transaction active? */
static int      jnl_inited;

/* Transaction buffer: cached metadata blocks before commit */
#define JNL_MAX_BLOCKS 64

static struct {
    uint64_t block_nr;
    uint8_t  data[4096];
} tx_buf[JNL_MAX_BLOCKS];
static int tx_count;

static spinlock_t jnl_lock = SPINLOCK_INIT;

/* ── Helpers ──────────────────────────────────────── */

/* Simple checksum: sum of all bytes */
static uint32_t checksum(const void *data, int len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    for (int i = 0; i < len; i++)
        sum += p[i];
    return sum;
}

/* Write one block to journal ring */
static int jnl_write_block(const void *data) {
    uint64_t blk = jnl_start + jnl_head;
    jnl_head = (jnl_head + 1) % jnl_size;
    return blk_write(blk, data);
}

/* ── Init ─────────────────────────────────────────── */

void journal_init(uint64_t start_block, uint32_t size_blocks) {
    jnl_start = start_block;
    jnl_size  = size_blocks;
    jnl_head  = 0;
    jnl_tx_id = 1;
    jnl_in_tx = 0;
    tx_count  = 0;
    jnl_inited = 1;

    serial_puts("journal: init (");
    { char t[12]; int i=0; uint32_t v=size_blocks;
      do{t[i++]='0'+(char)(v%10);v/=10;}while(v);
      while(i--) serial_putchar(t[i]); }
    serial_puts(" blocks)\n");
}

/* ── Transaction API ──────────────────────────────── */

int journal_begin(void) {
    if (!jnl_inited) return 0;  /* no journal = no-op (graceful) */
    if (jnl_in_tx) return 0;   /* nested begin = no-op */

    uint64_t flags;
    spin_lock_irq(&jnl_lock, &flags);
    jnl_in_tx = 1;
    tx_count = 0;
    spin_unlock_irq(&jnl_lock, flags);
    return 0;
}

int journal_write(uint64_t block, const void *data) {
    if (!jnl_inited || !jnl_in_tx) return 0;

    uint64_t flags;
    spin_lock_irq(&jnl_lock, &flags);

    if (tx_count >= JNL_MAX_BLOCKS) {
        spin_unlock_irq(&jnl_lock, flags);
        serial_puts("journal: tx full\n");
        return -1;
    }

    /* Check if this block is already in the transaction (coalesce) */
    for (int i = 0; i < tx_count; i++) {
        if (tx_buf[i].block_nr == block) {
            kmemcpy(tx_buf[i].data, data, 4096);
            spin_unlock_irq(&jnl_lock, flags);
            return 0;
        }
    }

    tx_buf[tx_count].block_nr = block;
    kmemcpy(tx_buf[tx_count].data, data, 4096);
    tx_count++;

    spin_unlock_irq(&jnl_lock, flags);
    return 0;
}

int journal_commit(void) {
    if (!jnl_inited || !jnl_in_tx) return 0;

    uint64_t flags;
    spin_lock_irq(&jnl_lock, &flags);

    if (tx_count == 0) {
        jnl_in_tx = 0;
        spin_unlock_irq(&jnl_lock, flags);
        return 0;
    }

    /* Check if journal has enough space:
     * Need: 1 (header) + tx_count * 2 (block_nr page + data page) + 1 (commit)
     * Actually: header and commit fit in partial blocks.
     * Simplified: 1 header block + tx_count data blocks + 1 commit block. */
    uint32_t needed = 2 + (uint32_t)tx_count;  /* header + data blocks + commit */
    if (needed > jnl_size) {
        serial_puts("journal: tx too large\n");
        jnl_in_tx = 0;
        tx_count = 0;
        spin_unlock_irq(&jnl_lock, flags);
        return -1;
    }

    uint32_t this_tx = jnl_tx_id++;

    /* Phase 1: Write transaction header block */
    uint8_t hdr_block[4096];
    kmemset(hdr_block, 0, 4096);
    /* Header: magic(4) tx_id(4) block_count(4) padding(4) */
    uint32_t magic = JOURNAL_TX_MAGIC;
    kmemcpy(hdr_block, &magic, 4);
    kmemcpy(hdr_block + 4, &this_tx, 4);
    uint32_t bc = (uint32_t)tx_count;
    kmemcpy(hdr_block + 8, &bc, 4);
    /* Block numbers list: 8 bytes each, starting at offset 16 */
    for (int i = 0; i < tx_count; i++) {
        kmemcpy(hdr_block + 16 + i * 8, &tx_buf[i].block_nr, 8);
    }
    jnl_write_block(hdr_block);

    /* Phase 2: Write data blocks */
    for (int i = 0; i < tx_count; i++) {
        jnl_write_block(tx_buf[i].data);
    }

    /* Phase 3: Write commit record */
    uint8_t commit_block[4096];
    kmemset(commit_block, 0, 4096);
    uint32_t cmag = JOURNAL_COMMIT_MAGIC;
    kmemcpy(commit_block, &cmag, 4);
    kmemcpy(commit_block + 4, &this_tx, 4);
    /* Checksum over header + all data blocks */
    uint32_t csum = checksum(hdr_block, 4096);
    for (int i = 0; i < tx_count; i++)
        csum += checksum(tx_buf[i].data, 4096);
    kmemcpy(commit_block + 8, &csum, 4);
    jnl_write_block(commit_block);

    /* Phase 4: Checkpoint — write blocks to final locations via bcache */
    for (int i = 0; i < tx_count; i++) {
        bcache_write_block(tx_buf[i].block_nr, tx_buf[i].data);
    }

    jnl_in_tx = 0;
    tx_count = 0;

    spin_unlock_irq(&jnl_lock, flags);
    return 0;
}

int journal_active(void) {
    return jnl_in_tx;
}

/* ── Recovery ─────────────────────────────────────── */

void journal_recover(void) {
    if (!jnl_inited) return;

    serial_puts("journal: scanning for recovery...\n");

    /* Scan journal blocks looking for valid transaction headers */
    uint8_t buf[4096];
    int recovered = 0;

    for (uint32_t pos = 0; pos < jnl_size; ) {
        if (blk_read(jnl_start + pos, buf) < 0) { pos++; continue; }

        uint32_t magic;
        kmemcpy(&magic, buf, 4);
        if (magic != JOURNAL_TX_MAGIC) { pos++; continue; }

        uint32_t tx_id, block_count;
        kmemcpy(&tx_id, buf + 4, 4);
        kmemcpy(&block_count, buf + 8, 4);

        if (block_count == 0 || block_count > JNL_MAX_BLOCKS) { pos++; continue; }
        if (pos + 1 + block_count + 1 > jnl_size) { pos++; continue; }

        /* Read block numbers from header */
        uint64_t block_nrs[JNL_MAX_BLOCKS];
        for (uint32_t i = 0; i < block_count; i++)
            kmemcpy(&block_nrs[i], buf + 16 + i * 8, 8);

        /* Compute expected checksum */
        uint32_t csum = checksum(buf, 4096);

        /* Read data blocks */
        uint8_t data_blocks[JNL_MAX_BLOCKS][4096];
        int valid = 1;
        for (uint32_t i = 0; i < block_count; i++) {
            if (blk_read(jnl_start + pos + 1 + i, data_blocks[i]) < 0) {
                valid = 0;
                break;
            }
            csum += checksum(data_blocks[i], 4096);
        }

        if (!valid) { pos++; continue; }

        /* Check commit record */
        uint8_t commit_buf[4096];
        if (blk_read(jnl_start + pos + 1 + block_count, commit_buf) < 0) { pos++; continue; }

        uint32_t cmag, ctx_id, stored_csum;
        kmemcpy(&cmag, commit_buf, 4);
        kmemcpy(&ctx_id, commit_buf + 4, 4);
        kmemcpy(&stored_csum, commit_buf + 8, 4);

        if (cmag == JOURNAL_COMMIT_MAGIC && ctx_id == tx_id && stored_csum == csum) {
            /* Valid transaction — replay blocks to final locations */
            for (uint32_t i = 0; i < block_count; i++) {
                blk_write(block_nrs[i], data_blocks[i]);
            }
            recovered++;
        }

        pos += 2 + block_count;  /* skip past this transaction */
    }

    if (recovered > 0) {
        serial_puts("journal: recovered ");
        serial_putchar('0' + (char)(recovered % 10));
        serial_puts(" transactions\n");
    } else {
        serial_puts("journal: clean\n");
    }

    /* Reset journal head */
    jnl_head = 0;
}
