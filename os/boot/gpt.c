/*
 * Zeos — GPT reader
 *
 * Reads GPT headers and partition tables via block.c per-drive I/O.
 * Lifted from the inline parser that used to live in updater.c so the
 * shell, installer-verify, and any future partition-aware tooling can
 * share one implementation.
 */

#include "gpt.h"
#include "block.h"
#include "heap.h"
#include "kprint.h"

static int mem_eq(const void *a, const void *b, uint64_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (uint64_t i = 0; i < n; i++) if (pa[i] != pb[i]) return 0;
    return 1;
}

static void mem_set(void *dst, uint8_t v, uint64_t n) {
    uint8_t *p = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++) p[i] = v;
}

static void mem_copy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

const uint8_t GPT_TYPE_ESP[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};
const uint8_t GPT_TYPE_LINUX_FS[16] = {
    0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
    0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};
const uint8_t GPT_TYPE_MS_BASIC[16] = {
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};

int gpt_guid_eq(const uint8_t *a, const uint8_t *b) {
    return mem_eq(a, b, 16);
}

typedef struct __attribute__((packed)) {
    uint8_t  signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alt_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t part_entry_lba;
    uint32_t num_parts;
    uint32_t part_entry_size;
    uint32_t part_crc32;
} gpt_header_disk_t;

typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[GPT_PARTITION_NAME_LEN];
} gpt_entry_disk_t;

static const uint8_t GPT_SIG[8] = {'E','F','I',' ','P','A','R','T'};

static uint8_t *alloc_blk(int drive_idx, uint32_t *out_blk_size) {
    block_drive_info_t info;
    if (block_drive_info(drive_idx, &info) != 0) return 0;
    uint32_t bs = info.sector_size ? info.sector_size : 512;
    if (out_blk_size) *out_blk_size = bs;
    return (uint8_t *)kmalloc(bs);
}

int gpt_read_header(int drive_idx, struct gpt_header *out) {
    if (!out) return -1;
    uint32_t blk_size = 0;
    uint8_t *blk = alloc_blk(drive_idx, &blk_size);
    if (!blk) return -1;

    if (block_read_drive(drive_idx, 1, 1, blk) != 0) {
        kfree(blk);
        return -1;
    }

    gpt_header_disk_t *gpt = (gpt_header_disk_t *)blk;
    if (!mem_eq(gpt->signature, GPT_SIG, 8)) {
        kfree(blk);
        return -1;
    }

    mem_set(out, 0, sizeof(*out));
    mem_copy(out->signature, gpt->signature, 8);
    out->revision         = gpt->revision;
    out->header_size      = gpt->header_size;
    out->header_crc32     = gpt->header_crc32;
    out->reserved         = gpt->reserved;
    out->my_lba           = gpt->my_lba;
    out->alt_lba          = gpt->alt_lba;
    out->first_usable_lba = gpt->first_usable_lba;
    out->last_usable_lba  = gpt->last_usable_lba;
    mem_copy(out->disk_guid, gpt->disk_guid, 16);
    out->part_entry_lba   = gpt->part_entry_lba;
    out->num_parts        = gpt->num_parts;
    out->part_entry_size  = gpt->part_entry_size;
    out->part_crc32       = gpt->part_crc32;

    kfree(blk);
    return 0;
}

int gpt_read_partition(int drive_idx, int part_idx, struct gpt_partition *out) {
    if (!out || part_idx < 0) return -1;

    struct gpt_header hdr;
    if (gpt_read_header(drive_idx, &hdr) != 0) return -1;
    if ((uint32_t)part_idx >= hdr.num_parts) return -1;
    if (hdr.part_entry_size < sizeof(gpt_entry_disk_t)) return -1;

    uint32_t blk_size = 0;
    uint8_t *blk = alloc_blk(drive_idx, &blk_size);
    if (!blk) return -1;

    uint32_t entries_per_block = blk_size / hdr.part_entry_size;
    if (entries_per_block == 0) {
        kfree(blk);
        return -1;
    }

    uint32_t block_idx = (uint32_t)part_idx / entries_per_block;
    uint32_t entry_idx = (uint32_t)part_idx % entries_per_block;

    if (block_read_drive(drive_idx, hdr.part_entry_lba + block_idx, 1, blk) != 0) {
        kfree(blk);
        return -1;
    }

    gpt_entry_disk_t *ent =
        (gpt_entry_disk_t *)(blk + entry_idx * hdr.part_entry_size);

    mem_set(out, 0, sizeof(*out));
    out->index = part_idx;

    static const uint8_t zero_guid[16] = {0};
    if (mem_eq(ent->type_guid, zero_guid, 16)) {
        kfree(blk);
        return 1;   /* empty slot */
    }

    mem_copy(out->type_guid,   ent->type_guid,   16);
    mem_copy(out->unique_guid, ent->unique_guid, 16);
    out->first_lba   = ent->first_lba;
    out->last_lba    = ent->last_lba;
    out->size_blocks = (ent->last_lba >= ent->first_lba)
                        ? (ent->last_lba - ent->first_lba + 1) : 0;
    out->attributes  = ent->attributes;

    int term = 0;
    for (int i = 0; i < GPT_PARTITION_NAME_LEN; i++) {
        out->name_utf16[i] = ent->name[i];
        uint16_t c = ent->name[i];
        if (term) {
            out->name_ascii[i] = '\0';
            continue;
        }
        if (c == 0) {
            out->name_ascii[i] = '\0';
            term = 1;
            continue;
        }
        out->name_ascii[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    out->name_ascii[GPT_PARTITION_NAME_LEN] = '\0';

    kfree(blk);
    return 0;
}

int gpt_find_esp(int drive_idx) {
    struct gpt_header hdr;
    if (gpt_read_header(drive_idx, &hdr) != 0) return -1;

    for (uint32_t i = 0; i < hdr.num_parts; i++) {
        struct gpt_partition p;
        int rc = gpt_read_partition(drive_idx, (int)i, &p);
        if (rc < 0) return -1;
        if (rc == 1) continue;   /* empty slot */
        if (gpt_guid_eq(p.type_guid, GPT_TYPE_ESP)) return (int)i;
    }
    return -1;
}
