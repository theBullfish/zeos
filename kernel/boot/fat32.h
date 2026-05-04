/*
 * Zeos — FAT32 driver (read + write)
 *
 * General-purpose FAT32 driver for USB sticks, SD cards, NVMe images,
 * and the EFI System Partition. All writes pass through CHAIN_BLOCK,
 * so masq_journal records every disk-write sector.
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

#endif /* ZEOS_FAT32_H */
