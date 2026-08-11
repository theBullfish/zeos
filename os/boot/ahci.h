/*
 * Zeos — AHCI / SATA block device driver
 *
 * Polling-only driver. Targets SATA SSDs / HDDs on any x86_64 with an
 * AHCI-compliant controller (PCI class 0x01 subclass 0x06 prog-if 0x01)
 * — covers Intel/AMD/Marvell/etc circa 2008+.
 *
 * Multi-port: enumerates all 32 implemented ports on the first AHCI
 * controller, brings up every present+active SATA drive, exposes them
 * via ahci_drive_count() / per-drive accessors. Per-drive I/O via
 * ahci_read_drive() / ahci_write_drive() / ahci_flush_drive().
 *
 * Legacy single-drive API (ahci_read/write/flush, ahci_num_blocks,
 * ahci_block_size, ahci_ready) targets drive 0 — keeps existing
 * callers working unchanged.
 *
 * No NCQ, no interrupts, one outstanding command per port at a time.
 */

#ifndef ZEOS_AHCI_H
#define ZEOS_AHCI_H

#include <stdint.h>

/* Brings up every present SATA drive. Returns 0 if at least one drive
 * is ready, -1 if no AHCI controller / no ATA drives. */
int ahci_init(void);

/* ── Multi-drive API ───────────────────────────────────────── */

int      ahci_drive_count(void);
int      ahci_read_drive(int idx, uint64_t lba, uint32_t count, void *buf);
int      ahci_write_drive(int idx, uint64_t lba, uint32_t count, const void *buf);
int      ahci_flush_drive(int idx);
uint64_t ahci_drive_blocks(int idx);
uint32_t ahci_drive_block_size(int idx);
int      ahci_drive_port(int idx);

/* ── Legacy single-drive API (operates on drive 0) ────────── */

int ahci_read(uint64_t lba, uint32_t count, void *buf);
int ahci_write(uint64_t lba, uint32_t count, const void *buf);
int ahci_flush(void);

uint64_t ahci_num_blocks(void);
uint32_t ahci_block_size(void);
int      ahci_ready(void);

#endif /* ZEOS_AHCI_H */
