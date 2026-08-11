/*
 * Zeos -- USB Mass Storage Class (Bulk-Only Transport) driver
 *
 * Implements the USB MSC BBB protocol (USB-IF "Mass Storage Class —
 * Bulk-Only Transport" rev 1.0, sept 1999) on top of the xHCI bulk
 * endpoint API. SCSI command set: TEST UNIT READY, INQUIRY,
 * READ CAPACITY(10), READ(10), WRITE(10), REQUEST SENSE.
 *
 * Detects MSC devices by walking the configuration descriptor of every
 * xHCI-attached device looking for an interface with class=0x08 (Mass
 * Storage), subclass=0x06 (SCSI transparent), protocol=0x50 (BBB),
 * paired with one Bulk IN + one Bulk OUT endpoint.
 *
 * Plugs into the block-device dispatcher (block.c) as the third option
 * after NVMe + AHCI. Single-LUN only — enough for thumb drives.
 */

#ifndef ZEOS_USB_MSC_H
#define ZEOS_USB_MSC_H

#include <stdint.h>

/* Discover and bring up the first BBB MSC LUN0 found on the xHCI bus.
 * Returns 0 on success, negative on no device found / probe failure. */
int usb_msc_init(void);

/* True if a usable MSC LUN is present. */
int usb_msc_ready(void);

/* Number of 512-byte (or msc_block_size()) sectors on the device. */
uint64_t usb_msc_sector_count(void);

/* Sector size in bytes (typically 512). 0 if no device. */
uint32_t usb_msc_block_size(void);

/* Read `count` sectors starting at `lba` into `buf`. Returns 0 on success. */
int usb_msc_read(uint64_t lba, uint32_t count, void *buf);

/* Write `count` sectors starting at `lba` from `buf`. Returns 0 on success. */
int usb_msc_write(uint64_t lba, uint32_t count, const void *buf);

/* Flush is a no-op for MSC BBB (writes are synchronous through CSW). */
int usb_msc_flush(void);

#endif /* ZEOS_USB_MSC_H */
