/*
 * Zeos -- Block device dispatcher (multi-drive)
 */
#ifndef ZEOS_BLOCK_H
#define ZEOS_BLOCK_H
#include <stdint.h>

#define BLOCK_KIND_NONE    0
#define BLOCK_KIND_NVME    1
#define BLOCK_KIND_AHCI    2
#define BLOCK_KIND_USB_MSC 3

typedef struct {
    int idx;
    int kind;
    int sub_idx;
    uint64_t sectors;
    uint32_t sector_size;
    char serial[24];
    char model[44];
} block_drive_info_t;

int block_init(void);

int block_drive_count(void);
int block_drive_info(int idx, block_drive_info_t *out);

int block_read_drive(int idx, uint64_t lba, uint32_t count, void *buf);
int block_write_drive(int idx, uint64_t lba, uint32_t count, const void *buf);
int block_flush_drive(int idx);

int block_read(uint64_t lba, uint32_t count, void *buf);
int block_write(uint64_t lba, uint32_t count, const void *buf);
int block_flush(void);

uint32_t block_size(void);
const char *block_kind(void);

#endif
