/*
 * Zeos — FAT32 driver (read + write)
 *
 * General-purpose FAT32 driver for USB sticks, SD cards, NVMe images,
 * and the EFI System Partition. All writes pass through CHAIN_BLOCK,
 * so masq_journal records every disk-write sector.
 *
 * Trash: rm moves to /.zeos-trash/ by default; `rm -f` for direct
 * unlink. CHAIN_TRASH_GC auto-empties older than 30 days.
 */

#ifndef ZEOS_FAT32_H
#define ZEOS_FAT32_H

#include <stdint.h>

#define FAT32_NAME_MAX 255
#define FAT32_PATH_MAX 256

struct fat32_file {
    uint32_t start_cluster;
    uint32_t size;
    uint32_t cur_cluster;
    uint32_t cur_offset;
    uint8_t  is_dir;
};

struct fat32_dirent {
    char     name[FAT32_NAME_MAX + 1];
    uint32_t size;
    uint32_t start_cluster;
    uint8_t  is_dir;
    uint8_t  attr;
};

/* Read API */
int fat32_mount(int drive_idx, uint64_t partition_lba);
int fat32_automount(void);
int fat32_mounted(void);
int fat32_open(const char *path, struct fat32_file *out);
int fat32_read(struct fat32_file *file, void *buf, uint32_t max_len);
int fat32_list(const char *dir, struct fat32_dirent *entries, int max);

/* readdir-with-callback. Callback returns non-zero to stop. */
typedef int (*fat32_readdir_cb)(const char *name, uint32_t size,
                                uint32_t cluster, uint8_t attr,
                                void *user);
int fat32_readdir(const char *path, fat32_readdir_cb cb, void *user);

/* Write API. All writes go through block_write_drive -> CHAIN_BLOCK,
 * which masq_journal records sector-by-sector. Returns 0 on success,
 * negative on failure (fat32_write returns bytes written). */
int fat32_create(const char *path);
int fat32_write(const char *path, uint32_t offset,
                const void *data, uint32_t len);
int fat32_truncate(const char *path, uint32_t new_size);
int fat32_unlink(const char *path);
int fat32_mkdir(const char *path);

/* ── Trash API ──────────────────────────────────────────────────
 *
 * Soft-delete: a file at `path` is moved into `/.zeos-trash/<id>/`,
 * with a sibling `meta.txt` recording the original path, size,
 * deletion timestamp (Unix seconds via tod_now_unix()), and reason
 * ("user" / "auto"). The move re-points dir entries; cluster chains
 * are NOT copied, so trashing is O(1) regardless of file size.
 *
 * Restore re-points the dir entry back to the original path's
 * parent. Empty hard-unlinks every file inside every <id>/ subdir
 * and frees the cluster chains for real.
 *
 * /.zeos-trash/ is created lazily on first delete. Files beneath
 * /.zeos-trash/ are never themselves trashed — fat32_trash() refuses
 * paths inside the trash root.
 */

/* Move `path` to /.zeos-trash/<id>/. Returns 0 on success, -1 on
 * failure. The unique id is written into out_id (caller-supplied,
 * at least FAT32_TRASH_ID_MAX+1 bytes); pass NULL to ignore. The
 * `reason` argument is recorded in meta.txt; pass NULL for "user". */
#define FAT32_TRASH_ID_MAX 16
int fat32_trash(const char *path, const char *reason, char *out_id);

/* Enumerate trash entries. Callback receives id, original path,
 * deletion epoch seconds, and size (bytes). Returns the entry count
 * (>=0), or -1 on error. */
typedef void (*fat32_trash_list_cb)(const char *id,
                                    const char *orig_path,
                                    uint64_t deleted_at,
                                    uint32_t size);
int fat32_trash_list(fat32_trash_list_cb cb);

/* Restore the entry with the given id to its original path. Returns
 * 0 on success, -1 on failure (id not found, parent gone, etc.). */
int fat32_trash_restore(const char *id);

/* Permanently delete every trash entry. Returns the number of
 * entries freed, or -1 on error. */
int fat32_trash_empty(void);

/* Permanently delete entries older than `cutoff_secs` seconds (i.e.
 * entries whose deleted_at < tod_now_unix() - cutoff_secs). Returns
 * the number of entries freed, or -1 on error. */
int fat32_trash_empty_older_than(uint64_t cutoff_secs);

/* Counts + bytes accounting (selftest helper). out_count and
 * out_bytes may be NULL. Returns 0 on success. */
int fat32_trash_stats(uint32_t *out_count, uint64_t *out_bytes);

/* Ensures the trash root exists. Returns 0 on success or if the
 * directory already exists, -1 on failure. */
int fat32_trash_ensure_root(void);

#endif /* ZEOS_FAT32_H */
