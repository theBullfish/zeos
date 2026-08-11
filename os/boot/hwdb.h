/*
 * Zeos preloaded hardware knowledge base (see tools/gen_hwdb.py).
 *
 * Runtime DISCOVERY tells Zeos what a machine has; this tells it what those IDs
 * MEAN — offline, with no lookup service, on hardware it has never seen before.
 * All lookups are binary searches over generated tables. NULL = not in the db.
 */
#ifndef ZEOS_HWDB_H
#define ZEOS_HWDB_H

#include <stdint.h>

const char *hwdb_pci_vendor(uint16_t vid);
const char *hwdb_pci_device(uint16_t vid, uint16_t did);
const char *hwdb_usb_vendor(uint16_t vid);
const char *hwdb_usb_device(uint16_t vid, uint16_t did);

/* Standardized class meaning, most-specific first (triple -> subclass -> class). */
const char *hwdb_pci_class(uint8_t cls, uint8_t sub, uint8_t progif);

/* The protocol a device SPEAKS, from its class triple alone: "nvme", "ahci",
 * "xhci", "ehci", "ohci", "uhci", "ide", "sata", "sas", "ethernet", "display",
 * "hda", "bridge", "wireless", or NULL. Standards-based, so it binds correctly to
 * hardware that did not exist when this image was built — with no probing. */
const char *hwdb_pci_protocol(uint8_t cls, uint8_t sub, uint8_t progif);

int hwdb_pci_class_count(void);
int hwdb_pci_vendor_count(void);
int hwdb_pci_device_count(void);
int hwdb_usb_vendor_count(void);
int hwdb_usb_device_count(void);

#endif
