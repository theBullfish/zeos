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

/* Write a 32-bit value to PCI config space */
void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val);

/* Get human-readable class name */
const char *pci_class_name(uint8_t class_code, uint8_t subclass);

/* Check if ECAM (PCIe MMIO config) is available */
int ecam_available_check(void);

/* ── PCIe capability walking ─────────────────────────────── */

uint8_t pci_find_capability(struct pci_device *dev, uint8_t cap_id);
int pci_has_msi(struct pci_device *dev);
int pci_has_msix(struct pci_device *dev);
int pci_link_speed(struct pci_device *dev);   /* 1=Gen1 ... 6=Gen6, 0=N/A */
int pci_link_width(struct pci_device *dev);   /* x1, x4, x8, x16 ... */
const char *pci_link_speed_name(int speed_code);
uint64_t pci_bar_size(struct pci_device *dev, int bar_idx);
const char *pci_vendor_name(uint16_t vendor_id);
const char *pci_device_name(uint16_t vendor_id, uint16_t device_id);

#endif /* ZEOS_PCI_H */
