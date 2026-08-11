/*
 * Zeos -- VAULT On-Disk Persistence Layer
 *
 * Simple block-based filesystem on a dedicated partition.
 * NOT a general-purpose filesystem -- purpose-built for VAULT.
 *
 * Provides reboot persistence for:
 *   - B3 posteriors (chain learning survives power cycles)
 *   - Settings / config values
 *   - Desktop state (icons, layout)
 *
 * Uses block device callbacks (NVMe read/write) so this layer
 * is decoupled from any specific storage driver.
 *
 * On-disk layout:
 *   Block 0:       Superblock
 *   Blocks 1-N:    File table (vault_file_entry_t array)
 *   Blocks N+1...: Data blocks (sequential allocation)
 */

#ifndef ZEOS_VAULT_DISK_H
#define ZEOS_VAULT_DISK_H

#include <stdint.h>

/* ---- On-disk constants -------------------------------- */

#define VAULT_DISK_MAGIC    0x5641554C  /* 'VAUL' */
#define VAULT_DISK_VERSION  1
#define VAULT_MAX_FILES     256
#define VAULT_DISK_MAX_PATH 128
#define VAULT_DISK_BLOCK    4096

/* ---- On-disk structures ------------------------------- */

/*
 * Superblock -- block 0 of the VAULT partition.
 * Describes geometry and allocation state.
 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t total_blocks;
    uint32_t used_blocks;
    uint32_t file_count;
    uint32_t first_free_block;
    uint8_t  reserved[4072];        /* Pad to 4096 */
} vault_superblock_t;

/*
 * File entry -- stored in the file table region (blocks 1-N).
 * Each entry maps a path to a contiguous run of data blocks.
 */
typedef struct {
    char     path[VAULT_DISK_MAX_PATH];
    uint32_t start_block;
    uint32_t size_bytes;
    uint32_t version;               /* Temporal versioning -- incremented on each write */
    uint32_t timestamp;             /* Approximate (TSC-derived) */
    uint8_t  active;                /* 0 = deleted/empty, 1 = active */
    uint8_t  _pad[3];               /* Alignment */
} vault_file_entry_t;

/* ---- Block device callbacks --------------------------- */

/*
 * read_fn(lba, count, buf):  Read 'count' sectors starting at LBA into buf.
 * write_fn(lba, count, buf): Write 'count' sectors from buf starting at LBA.
 * Returns 0 on success, nonzero on error.
 *
 * LBA is absolute (partition_start_lba + offset is handled internally).
 */
typedef int (*vault_block_read_fn)(uint64_t lba, uint32_t count, void *buf);
typedef int (*vault_block_write_fn)(uint64_t lba, uint32_t count, const void *buf);

/* ---- API ---------------------------------------------- */

/*
 * Initialize on-disk VAULT.
 * Reads the superblock; if magic doesn't match, formats the partition.
 * Loads the file table into a static in-memory array.
 *
 * read_fn / write_fn:       Block device callbacks (NVMe, AHCI, etc.)
 * partition_start_lba:      First LBA of the VAULT partition
 * partition_blocks:         Size of the partition in VAULT_DISK_BLOCK-sized blocks
 *
 * Returns 0 on success, -1 on error.
 */
int vault_disk_init(vault_block_read_fn read_fn, vault_block_write_fn write_fn,
                    uint64_t partition_start_lba, uint64_t partition_blocks);

/*
 * Format the partition with an empty VAULT filesystem.
 * Called automatically by vault_disk_init if magic is wrong.
 * Can also be called explicitly for a clean wipe.
 *
 * Returns 0 on success, -1 on error.
 */
int vault_disk_format(void);

/*
 * Sync all in-memory VAULT entries to disk.
 * Walks the in-memory VAULT (vault_list / vault_read), serializes
 * each file to the on-disk format.
 *
 * Called on clean shutdown and periodically by the timer subsystem.
 *
 * Returns 0 on success, -1 on error.
 */
int vault_disk_sync(void);

/*
 * Load all files from disk VAULT into the in-memory VAULT.
 * For each active file entry on disk, calls vault_write() to
 * populate the RAM-backed filesystem.
 *
 * Called once at boot after vault_disk_init + vault_mount.
 *
 * Returns number of files loaded, or -1 on error.
 */
int vault_disk_load(void);

/*
 * Write a single file to the on-disk VAULT.
 * If the path already exists, the entry is updated in place
 * (version incremented, blocks reused or extended).
 * If new, allocates a free slot and sequential data blocks.
 *
 * Returns 0 on success, -1 on error.
 */
int vault_disk_write(const char *path, const void *data, uint32_t size);

/*
 * Read a single file from the on-disk VAULT.
 * Copies up to max_size bytes into buf.
 *
 * Returns the number of bytes read, or -1 if not found.
 */
int vault_disk_read(const char *path, void *buf, uint32_t max_size);

/*
 * Check if a file exists on the on-disk VAULT.
 *
 * Returns 1 if exists, 0 if not.
 */
int vault_disk_exists(const char *path);

#endif /* ZEOS_VAULT_DISK_H */
