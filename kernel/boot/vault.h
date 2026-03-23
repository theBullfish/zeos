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

#endif /* ZEOS_VAULT_H */
