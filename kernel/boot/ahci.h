/*
 * Zeos — AHCI / SATA block device driver
 *
 * Polling-only minimal driver. Targets SATA SSDs / HDDs on any
 * x86_64 with an AHCI-compliant controller (PCI class 0x01 subclass
 * 0x06 prog-if 0x01) — covers Intel/AMD/Marvell/etc circa 2008+.
 *
 * Single-port: we use the first ATA-capable port we find. No NCQ, no
 * interrupts, one outstanding command at a time.
 *
 * Mirrors the NVMe API: int read/write(lba, count, buf).
 */

#ifndef ZEOS_AHCI_H
#define ZEOS_AHCI_H

#include <stdint.h>

/* Returns 0 on success, -1 if no AHCI controller / no ATA drive. */
int ahci_init(void);

int ahci_read(uint64_t lba, uint32_t count, void *buf);
int ahci_write(uint64_t lba, uint32_t count, const void *buf);
int ahci_flush(void);

uint64_t ahci_num_blocks(void);
uint32_t ahci_block_size(void);
int      ahci_ready(void);

#endif /* ZEOS_AHCI_H */
