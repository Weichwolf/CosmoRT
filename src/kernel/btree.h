/* CosmoRT B+ Tree — persistent, on-disk, variable-length keys
 *
 * Used for directories (key=filename, value=inode) and attribute indices.
 * Each node occupies one 4KB disk block.
 *
 * Node layout:
 *   [header: type(1), count(2), padding(1)]   = 4 bytes
 *   [keys: count × (keylen(2) + key_data(var))]
 *   [values: count × uint64_t]                 (leaf only)
 *   [children: (count+1) × uint64_t]           (internal only)
 *
 * Leaf and internal nodes distinguished by type field.
 */
#ifndef BTREE_H
#define BTREE_H

#include <stdint.h>

#define BTREE_LEAF     1
#define BTREE_INTERNAL 2

#define BTREE_MAX_KEY  255  /* max key length in bytes */

/* Search for key in tree rooted at root_block.
 * Returns 0 and sets *value on success, -ENOENT if not found. */
int btree_search(uint64_t root_block, const char *key, uint64_t *value);

/* Insert key/value into tree. *root_block may change on root split.
 * Returns 0 on success. */
int btree_insert(uint64_t *root_block, const char *key, uint64_t value);

/* Delete key from tree. *root_block may change on root merge.
 * Returns 0 on success, -ENOENT if not found. */
int btree_delete(uint64_t *root_block, const char *key);

/* Iterate entries in sorted order. Calls cb for each key/value pair.
 * offset: skip first N entries. cb returns 0 to continue, nonzero to stop.
 * Returns number of entries visited. */
int btree_iterate(uint64_t root_block, int offset,
                  int (*cb)(const char *key, uint64_t value, void *ctx), void *ctx);

#endif
