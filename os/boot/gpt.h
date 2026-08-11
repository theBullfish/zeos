/*
 * Zeos — GPT (GUID Partition Table) reader
 *
 * Reusable public API for parsing GPT headers and partition tables on
 * any block device exposed by block.c. Operates per-drive so multi-drive
 * systems (NVMe + SATA + USB) can each be inspected independently.
 *
 * Read-only. No writing, no protective MBR generation. The installer
 * still owns disk creation; this module is for everything that reads.
 *
 * Lifted from updater.c so the shell, installer-verify, and any future
 * partition-aware tooling can share one implementation.
 */

#ifndef ZEOS_GPT_H
#define ZEOS_GPT_H

#include <stdint.h>

#define GPT_PARTITION_NAME_LEN 36   /* UTF-16LE chars in GPT entry */

struct gpt_header {
    uint8_t  signature[8];          /* "EFI PART" */
    uint32_t revision;
    uint32_t header_size;           /* Usually 92 */
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alt_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t part_entry_lba;
    uint32_t num_parts;
    uint32_t part_entry_size;       /* Usually 128 */
    uint32_t part_crc32;
};

struct gpt_partition {
    int       index;                /* 0-based partition index */
    uint8_t   type_guid[16];
    uint8_t   unique_guid[16];
    uint64_t  first_lba;
    uint64_t  last_lba;             /* Inclusive */
    uint64_t  size_blocks;          /* last_lba - first_lba + 1 */
    uint64_t  attributes;
    uint16_t  name_utf16[GPT_PARTITION_NAME_LEN];
    char      name_ascii[GPT_PARTITION_NAME_LEN + 1];
};

/* Read and validate the GPT header on the given block-layer drive.
 * Returns 0 on success, -1 if the drive is missing or the signature
 * doesn't match. CRC not yet verified — Alpha. */
int gpt_read_header(int drive_idx, struct gpt_header *out);

/* Read partition entry `part_idx` (0-based) from the given drive.
 * Returns 0 on success, 1 if the entry is empty (out left zeroed
 * except index), -1 on I/O error. Caller iterates 0..num_parts-1. */
int gpt_read_partition(int drive_idx, int part_idx, struct gpt_partition *out);

/* Find the EFI System Partition on the given drive. Returns the
 * partition index (0-based) on success, -1 if not found / I/O error. */
int gpt_find_esp(int drive_idx);

/* Compare two 16-byte GUIDs. */
int gpt_guid_eq(const uint8_t *a, const uint8_t *b);

/* Well-known partition type GUIDs (GPT mixed-endian binary form). */
extern const uint8_t GPT_TYPE_ESP[16];          /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
extern const uint8_t GPT_TYPE_LINUX_FS[16];     /* 0FC63DAF-8483-4772-8E79-3D69D8477DE4 */
extern const uint8_t GPT_TYPE_MS_BASIC[16];     /* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 */

#endif /* ZEOS_GPT_H */
