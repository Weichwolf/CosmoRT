/* CosmoRT Journal — metadata-only write-ahead log
 *
 * Ring buffer on disk. Wraps all metadata mutations (inode, bitmap, btree)
 * to guarantee consistency on crash recovery.
 *
 * Transaction format on disk:
 *   [TxHeader: magic(4), tx_id(4), block_count(4), padding(4)]  = 16 bytes
 *   [Block entries: (block_nr(8) + 4KB data) × block_count]
 *   [TxCommit: magic(4), tx_id(4), checksum(4), padding(4)]     = 16 bytes
 *
 * All packed within 4KB journal blocks.
 */
#ifndef JOURNAL_H
#define JOURNAL_H

#include <stdint.h>

#define JOURNAL_TX_MAGIC     0x4A584254  /* "TBXJ" */
#define JOURNAL_COMMIT_MAGIC 0x544D4F43  /* "COMT" */

void journal_init(uint64_t start_block, uint32_t size_blocks);

/* Start a transaction. Nested calls are no-ops (single tx at a time). */
int journal_begin(void);

/* Log a metadata block write. Block is written to journal, not to final location yet. */
int journal_write(uint64_t block, const void *data);

/* Commit transaction: write commit record, then replay blocks to final locations. */
int journal_commit(void);

/* Recovery at boot: replay any complete but not-yet-applied transactions. */
void journal_recover(void);

/* Is a transaction currently active? */
int journal_active(void);

#endif
