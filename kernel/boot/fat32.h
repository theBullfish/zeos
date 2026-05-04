/*
 * Zeos — Read-only FAT32 driver
 *
 * General-purpose FAT32 reader for USB sticks, SD cards, and the EFI
 * System Partition. The updater has its own inline FAT32 (kept for the
 * write path that places BOOTZ.EFI into the ESP); this driver is the
 * read-side surface used by the kernel and shell.
 *
 * Scope (Alpha):
 *   - Read-only. No writes, no allocation, no rename, no delete.
 *   - 8.3 short names AND VFAT long-name (LFN) entries.
 *   - Cluster chain walking via FAT1 (FAT2 ignored — we never write).
 *   - One mount slot. fat32_mount() replaces any prior mount.
 *
 * Path conventions:
 *   - Forward-slash separated: "/EFI/BOOT/BOOTX64.EFI"
 *   - Case-insensitive matching against both 8.3 names and LFN names.
 *   - Leading slash optional. "" or "/" means root.
 */

#ifndef ZEOS_FAT32_H
#define ZEOS_FAT32_H

#include <stdint.h>

#define FAT32_NAME_MAX 255
#define FAT32_PATH_MAX 256

struct fat32_file {
    uint32_t start_cluster;     /* First cluster of file */
    uint32_t size;              /* File size in bytes */
    uint32_t cur_cluster;       /* Current cluster (during read) */
    uint32_t cur_offset;        /* Bytes already read */
    uint8_t  is_dir;            /* 1 = directory, 0 = regular file */
};

struct fat32_dirent {
    char     name[FAT32_NAME_MAX + 1];
    uint32_t size;
    uint32_t start_cluster;
    uint8_t  is_dir;
    uint8_t  attr;              /* Raw FAT attribute byte */
};

/*
 * Mount a FAT32 partition.
 *   drive_idx     — reserved for future multi-drive support; the
 *                   current block API exposes one active device.
 *   partition_lba — first LBA of the partition. 0 for unpartitioned
 *                   media (USB sticks formatted whole-disk FAT32).
 * Returns 0 on success, -1 if the BPB is not FAT32.
 */
int fat32_mount(int drive_idx, uint64_t partition_lba);

/* Auto-mount: scan the active block device for a FAT32 signature
 * (whole-disk first, then GPT partition 1). Returns 0 on success. */
int fat32_automount(void);

/* True if a FAT32 volume is currently mounted. */
int fat32_mounted(void);

/* Open a file by absolute path. Fills *out on success. */
int fat32_open(const char *path, struct fat32_file *out);

/* Read up to max_len bytes. Returns bytes read, 0 at EOF, <0 on error.
 * Subsequent calls continue from the previous offset. */
int fat32_read(struct fat32_file *file, void *buf, uint32_t max_len);

/* List directory entries. Returns count written (0..max), or -1. */
int fat32_list(const char *dir, struct fat32_dirent *entries, int max);

#endif /* ZEOS_FAT32_H */
