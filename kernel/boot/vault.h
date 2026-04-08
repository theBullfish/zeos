/*
 * Zeos — VAULT Filesystem
 *
 * Temporal-state storage. Every write preserves history.
 * Not a filesystem with database features.
 * A fractal database with a filesystem interface.
 *
 * On-disk format:
 *   Superblock → Block bitmap → Inode table → Data blocks
 *
 * Each file is a chain of versioned extents.
 * Each version links to the previous (append-only history).
 * Deletion marks as invisible — data persists in temporal history.
 *
 * Three tiers (from VAULT spec):
 *   Sovereign  — user's data, encrypted at rest, never leaves device
 *   Internal   — app data, scoped to process
 *   Reference  — shared/public data, read-optimized
 */

#ifndef ZEOS_VAULT_H
#define ZEOS_VAULT_H

#include <stdint.h>

/* ── On-disk constants ───────────────────────── */

#define VAULT_MAGIC         0x564C5421      /* "VLT!" */
#define VAULT_VERSION       1
#define VAULT_BLOCK_SIZE    4096
#define VAULT_MAX_NAME      60
#define VAULT_MAX_PATH      256
#define VAULT_INODE_COUNT   1024
#define VAULT_DIRECT_BLOCKS 12

/* Tier levels */
#define VAULT_TIER_SOVEREIGN  0     /* User data, encrypted */
#define VAULT_TIER_INTERNAL   1     /* App data, scoped */
#define VAULT_TIER_REFERENCE  2     /* Shared/public */

/* File types */
#define VAULT_TYPE_FREE       0
#define VAULT_TYPE_FILE       1
#define VAULT_TYPE_DIR        2
#define VAULT_TYPE_LINK       3
#define VAULT_TYPE_SIGNAL     4     /* Live signal chain endpoint */

/* ── On-disk structures ──────────────────────── */

/*
 * Superblock — first block on disk.
 * Describes the filesystem geometry.
 */
struct vault_super {
    uint32_t    magic;              /* VAULT_MAGIC */
    uint32_t    version;            /* VAULT_VERSION */
    uint32_t    block_size;         /* Always 4096 for now */
    uint32_t    total_blocks;       /* Total blocks on device */
    uint32_t    free_blocks;        /* Free block count */
    uint32_t    inode_count;        /* Total inodes */
    uint32_t    free_inodes;        /* Free inode count */
    uint32_t    bitmap_start;       /* Block number of bitmap start */
    uint32_t    inode_start;        /* Block number of inode table start */
    uint32_t    data_start;         /* Block number of first data block */
    uint32_t    root_inode;         /* Inode number of root directory */
    uint64_t    created_tsc;        /* TSC at format time */
    uint64_t    mount_count;        /* Number of mounts */
    uint8_t     label[32];          /* Volume label */
    uint8_t     _reserved[3960];    /* Pad to 4096 bytes */
};

/*
 * Inode — file metadata + temporal history chain.
 *
 * Each inode has a pointer to its previous version.
 * Overwriting a file creates a NEW inode version that
 * links back to the old one. The old data is still
 * accessible via vault.read(path, at: t-1).
 *
 * This is append-only. The fractal database never forgets.
 */
struct vault_inode {
    uint32_t    type;               /* VAULT_TYPE_* */
    uint32_t    tier;               /* VAULT_TIER_* */
    uint32_t    size;               /* File size in bytes */
    uint32_t    blocks;             /* Number of data blocks used */
    uint32_t    direct[VAULT_DIRECT_BLOCKS]; /* Direct block pointers */
    uint32_t    indirect;           /* Single indirect block pointer */
    uint64_t    created_tsc;        /* TSC at creation */
    uint64_t    modified_tsc;       /* TSC at last modification */
    uint32_t    prev_version;       /* Inode number of previous version (0 = none) */
    uint32_t    version;            /* Version counter */
    uint32_t    owner_pid;          /* Process that owns this (for INTERNAL tier) */
    uint32_t    flags;              /* Permissions, immutable, etc */
    uint8_t     _reserved[8];       /* Pad to 128 bytes */
};

/*
 * Directory entry — maps a name to an inode.
 * 64 bytes each, so 64 entries per 4K block.
 */
struct vault_dirent {
    uint32_t    inode;              /* Inode number (0 = empty slot) */
    char        name[VAULT_MAX_NAME]; /* Null-terminated name */
};

/* ── In-memory state ─────────────────────────── */

struct vault_fs {
    struct vault_super  super;
    uint8_t            *bitmap;         /* Block allocation bitmap (in memory) */
    struct vault_inode *inodes;         /* Inode table (in memory) */
    int                 mounted;        /* Is filesystem mounted? */
    uint32_t            device_blocks;  /* Total blocks on backing device */
};

/* ── API ─────────────────────────────────────── */

/*
 * Format a region of memory as a VAULT filesystem.
 * base: start of the memory region
 * size: size in bytes
 * Returns 0 on success.
 */
int vault_format(void *base, uint64_t size, const char *label);

/*
 * Mount a VAULT filesystem from a memory region.
 * Returns 0 on success.
 */
int vault_mount(void *base, uint64_t size);

/*
 * Create a file. Returns inode number, or -1 on error.
 * Creates parent directories as needed.
 */
int vault_create(const char *path, uint32_t tier);

/*
 * Create a directory. Returns inode number, or -1 on error.
 * Creates parent directories as needed.
 */
int vault_mkdir(const char *path, uint32_t tier);

/*
 * Write data to a file. Creates a new version (temporal history).
 * Returns bytes written, or -1 on error.
 */
int vault_write(const char *path, const void *data, uint32_t size);

/*
 * Read data from a file.
 * buf: destination buffer
 * size: max bytes to read
 * Returns bytes read, or -1 on error.
 */
int vault_read(const char *path, void *buf, uint32_t size);

/*
 * Read a previous version of a file.
 * version: 0 = current, 1 = previous, 2 = two versions ago, etc.
 * Returns bytes read, or -1 on error.
 */
int vault_read_version(const char *path, void *buf, uint32_t size, uint32_t version);

/*
 * List directory contents.
 * entries: output array of dirents
 * max_entries: max entries to return
 * Returns number of entries, or -1 on error.
 */
int vault_list(const char *path, struct vault_dirent *entries, int max_entries);

/*
 * Append data to a file (no new version — same inode, extend).
 * Used for logs, audit trails, sensor recordings.
 * Returns bytes written, or -1 on error.
 */
int vault_append(const char *path, const void *data, uint32_t size);

/*
 * Delete a file. Marks as invisible but data persists in history.
 * Returns 0 on success, -1 on error.
 */
int vault_delete(const char *path);

/*
 * Get file size. Returns -1 if not found.
 */
int vault_size(const char *path);

/*
 * Check if path exists. Returns 1 if exists, 0 if not.
 */
int vault_exists(const char *path);

/*
 * Get filesystem stats.
 */
void vault_stat(uint32_t *total_blocks, uint32_t *free_blocks,
                uint32_t *total_inodes, uint32_t *free_inodes);

/*
 * Sync — flush all in-memory state to backing store.
 */
void vault_sync(void);

/* ── Chain Persistence ──────────────────────────── */

/*
 * Serialized chain state header.
 * Stored at /chains/[chain_name].state
 *
 * Binary format:
 *   vault_chain_header_t
 *   [name_len bytes of chain name]
 *   [node_count entries of vault_chain_node_t]
 */
#define VAULT_CHAIN_MAGIC   0x5A434853  /* 'ZCHS' */

typedef struct {
    uint32_t magic;             /* VAULT_CHAIN_MAGIC */
    uint32_t version;           /* Incremented on each save */
    float    b3_alpha;          /* B3 posterior alpha */
    float    b3_beta;           /* B3 posterior beta */
    int32_t  b3_observations;   /* Total B3 observations */
    uint8_t  tier;              /* MasQ tier */
    uint8_t  node_count;        /* Number of processing nodes */
    uint16_t name_len;          /* Length of chain name */
    uint64_t last_resolve_tsc;  /* TSC of last resolve */
    uint32_t cfa_segments[8];   /* CFA address segments */
    uint8_t  cfa_depth;         /* CFA depth */
    uint8_t  _pad[3];           /* Alignment padding */
} vault_chain_header_t;

/*
 * Serialized chain node (name + types only, no function pointers).
 */
typedef struct {
    char name[32];
    char input_type[32];
    char output_type[32];
} vault_chain_node_t;

/* Forward declare chain_t (defined in chain.h) */
struct chain;

/*
 * Save a chain's critical state to VAULT.
 * Stores at /chains/[chain_name].state
 * Increments vault_version in the chain on each save.
 * Returns 0 on success, -1 on error.
 */
int vault_save_chain_state(int chain_id);

/*
 * Load chain state from VAULT by name.
 * Restores B3 priors, CFA, MasQ tier, node info.
 * Returns 0 on success, -1 if not found.
 */
int vault_load_chain_state(const char *chain_name, struct chain *out);

/* ── Config Persistence ─────────────────────────── */

/*
 * Save a config value to VAULT.
 * Stores at /config/[key]
 * Returns bytes written, or -1 on error.
 */
int vault_save_config(const char *key, const void *value, uint32_t len);

/*
 * Load a config value from VAULT.
 * Returns bytes read, or -1 if not found.
 */
int vault_load_config(const char *key, void *value, uint32_t max_len);

/*
 * Check if a config key exists.
 * Returns 1 if exists, 0 if not.
 * Useful for first-boot detection.
 */
int vault_has_config(const char *key);

#endif /* ZEOS_VAULT_H */
