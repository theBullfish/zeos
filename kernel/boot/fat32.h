/*
 * Zeos — Read-only FAT32 driver
 *
 * General-purpose FAT32 reader for USB sticks, SD cards, and the EFI
 * System Partition.
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

int fat32_mount(int drive_idx, uint64_t partition_lba);
int fat32_automount(void);
int fat32_mounted(void);
int fat32_open(const char *path, struct fat32_file *out);
int fat32_read(struct fat32_file *file, void *buf, uint32_t max_len);
int fat32_list(const char *dir, struct fat32_dirent *entries, int max);

#endif /* ZEOS_FAT32_H */
