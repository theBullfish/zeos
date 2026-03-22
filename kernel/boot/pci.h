/*
 * Zeos — PCIe bus enumeration
 *
 * Walks the PCI configuration space and reports all devices.
 * Uses ACPI MCFG for PCIe enhanced config (MMIO) if available,
 * falls back to legacy I/O ports 0xCF8/0xCFC.
 */

#ifndef ZEOS_PCI_H
#define ZEOS_PCI_H

#include <stdint.h>

struct pci_device {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;
    uint32_t bar[6];
};

#define PCI_MAX_DEVICES 128

/* Initialize PCI subsystem (parse MCFG if available) */
void pci_init(void *rsdp);

/* Enumerate all PCI devices. Returns count found. */
int pci_enumerate(void);

/* Get device at index (0-based). Returns NULL if out of range. */
struct pci_device *pci_get_device(int index);

/* Get total device count from last enumeration */
int pci_device_count(void);

/* Read a 32-bit value from PCI config space */
uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);

/* Get human-readable class name */
const char *pci_class_name(uint8_t class_code, uint8_t subclass);

/* Check if ECAM (PCIe MMIO config) is available */
int ecam_available_check(void);

#endif /* ZEOS_PCI_H */
