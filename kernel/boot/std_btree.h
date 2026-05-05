/*
 * Zeos — std.btree
 *
 * In-memory B-tree with split / merge.  4 KiB pages, fanout up to 64.
 * Keys are int32_t; values are int32_t (callers use string-pool handles
 * for variable-size data).
 *
 * Persistence is the caller's job; a vault snapshot helper is on the
 * roadmap, not in this pass.
 */

#ifndef ZEOS_STD_BTREE_H
#define ZEOS_STD_BTREE_H

#include <stdint.h>

#define BTREE_MAX_TREES   16
#define BTREE_MAX_NODES   1024
#define BTREE_FANOUT      64
#define BTREE_MIN_KEYS    (BTREE_FANOUT / 2 - 1)

typedef int btree_handle_t;

/* Initialize the global btree pool. Idempotent. */
void btree_init(void);

/* Create a new empty tree.  Returns a handle, or -1 on out-of-pool. */
btree_handle_t btree_new(void);

/* Insert (key, value).  Replaces existing value if key is present.
 * Returns 0 on success, -1 on full-tree / bad-handle. */
int btree_insert(btree_handle_t h, int32_t key, int32_t value);

/* Look up.  Returns the stored value if present, 0 if missing.  Use
 * btree_contains to disambiguate. */
int32_t btree_get(btree_handle_t h, int32_t key);

/* Returns 1 if key present, 0 otherwise. */
int btree_contains(btree_handle_t h, int32_t key);

/* Delete a key. Returns 0 on success, -1 on miss / bad handle. */
int btree_delete(btree_handle_t h, int32_t key);

/* Walk every key in [lo, hi] inclusive in sorted order. Returns the
 * number of keys visited. */
int btree_range(btree_handle_t h, int32_t lo, int32_t hi,
                void (*visit)(int32_t key, int32_t value, void *ud),
                void *ud);

/* Count of trees currently allocated. */
int btree_tree_count(void);

#endif /* ZEOS_STD_BTREE_H */
