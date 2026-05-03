/*
 * Zeos — Block device dispatcher
 *
 * Thin wrapper that picks NVMe or AHCI/SATA at boot. The updater and
 * any future installer use these instead of nvme_* / ahci_* directly.
 */

#ifndef ZEOS_BLOCK_H
#define ZEOS_BLOCK_H

#include <stdint.h>

/* Try NVMe first, then AHCI. Returns 0 if either succeeds. */
int block_init(void);

int block_read(uint64_t lba, uint32_t count, void *buf);
int block_write(uint64_t lba, uint32_t count, const void *buf);
int block_flush(void);

uint32_t block_size(void);   /* sector size, typically 512 */
const char *block_kind(void); /* "nvme" / "ahci" / "none" */

#endif /* ZEOS_BLOCK_H */
