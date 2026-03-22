/*
 * Zeos — PCI/PCIe bus enumeration
 *
 * Two access methods:
 * 1. PCIe ECAM (Enhanced Configuration Access Mechanism) via ACPI MCFG
 *    - MMIO access to full 4096-byte config space per function
 * 2. Legacy PCI config I/O ports 0xCF8/0xCFC
 *    - Only 256 bytes of config space, but works everywhere
 */

#include "pci.h"
#include "io.h"
#include "fb.h"

/* ACPI table signatures */
#define ACPI_SIG_RSDP  0x2052545020445352ULL  /* "RSD PTR " */
#define ACPI_SIG_MCFG  0x4746434D            /* "MCFG" */

/* ACPI RSDP (v2) */
struct acpi_rsdp {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    /* v2 fields */
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

/* ACPI SDT header */
struct acpi_sdt {
    uint32_t signature;
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* MCFG allocation entry */
struct mcfg_entry {
    uint64_t base_addr;
    uint16_t segment;
    uint8_t  start_bus;
    uint8_t  end_bus;
    uint32_t reserved;
} __attribute__((packed));

/* MCFG table */
struct acpi_mcfg {
    struct acpi_sdt header;
    uint64_t reserved;
    struct mcfg_entry entries[];
} __attribute__((packed));

/* State */
static uint64_t ecam_base;
static int ecam_available;
static struct pci_device devices[PCI_MAX_DEVICES];
static int device_count;

/*
 * Legacy PCI config space access via I/O ports.
 */
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static uint32_t pci_legacy_read32(uint8_t bus, uint8_t dev, uint8_t func,
                                   uint8_t offset)
{
    uint32_t addr = (1U << 31)                /* Enable bit */
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8)
                  | (offset & 0xFC);

    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

/*
 * ECAM (MMIO) config space access.
 */
static uint32_t pci_ecam_read32(uint8_t bus, uint8_t dev, uint8_t func,
                                 uint16_t offset)
{
    uint64_t addr = ecam_base
                  + ((uint64_t)bus << 20)
                  + ((uint64_t)dev << 15)
                  + ((uint64_t)func << 12)
                  + offset;

    return *(volatile uint32_t *)addr;
}

uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    if (ecam_available)
        return pci_ecam_read32(bus, dev, func, offset);
    return pci_legacy_read32(bus, dev, func, offset);
}

/*
 * Parse ACPI tables to find MCFG.
 */
static void find_mcfg(void *rsdp_ptr)
{
    ecam_available = 0;
    ecam_base = 0;

    if (!rsdp_ptr)
        return;

    struct acpi_rsdp *rsdp = (struct acpi_rsdp *)rsdp_ptr;

    /* Use XSDT if revision >= 2, otherwise RSDT */
    struct acpi_sdt *sdt;
    int entry_size;
    int is_xsdt;

    if (rsdp->revision >= 2 && rsdp->xsdt_addr) {
        sdt = (struct acpi_sdt *)(uintptr_t)rsdp->xsdt_addr;
        entry_size = 8;
        is_xsdt = 1;
    } else {
        sdt = (struct acpi_sdt *)(uintptr_t)rsdp->rsdt_addr;
        entry_size = 4;
        is_xsdt = 0;
    }

    /* Walk table entries */
    int num_entries = (sdt->length - sizeof(struct acpi_sdt)) / entry_size;
    uint8_t *entries = (uint8_t *)sdt + sizeof(struct acpi_sdt);

    for (int i = 0; i < num_entries; i++) {
        uint64_t table_addr;
        if (is_xsdt) {
            table_addr = *(uint64_t *)(entries + i * 8);
        } else {
            table_addr = *(uint32_t *)(entries + i * 4);
        }

        struct acpi_sdt *table = (struct acpi_sdt *)(uintptr_t)table_addr;

        if (table->signature == ACPI_SIG_MCFG) {
            struct acpi_mcfg *mcfg = (struct acpi_mcfg *)table;
            if (mcfg->header.length > sizeof(struct acpi_mcfg)) {
                ecam_base = mcfg->entries[0].base_addr;
                ecam_available = 1;
                return;
            }
        }
    }
}

void pci_init(void *rsdp)
{
    /*
     * Skip MCFG/ECAM for now — ACPI table walking after ExitBootServices
     * can hit unmapped memory. Legacy I/O always works.
     * TODO: Map ACPI regions properly before enabling ECAM.
     */
    (void)rsdp;
    ecam_available = 0;
    ecam_base = 0;
    device_count = 0;
}

/*
 * Scan a single bus/device/function.
 */
static void pci_scan_function(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t id = pci_config_read32(bus, dev, func, 0x00);
    uint16_t vendor = id & 0xFFFF;
    uint16_t device = (id >> 16) & 0xFFFF;

    if (vendor == 0xFFFF || vendor == 0x0000)
        return;

    if (device_count >= PCI_MAX_DEVICES)
        return;

    struct pci_device *d = &devices[device_count];
    d->bus       = bus;
    d->dev       = dev;
    d->func      = func;
    d->vendor_id = vendor;
    d->device_id = device;

    uint32_t class_reg = pci_config_read32(bus, dev, func, 0x08);
    d->class_code  = (class_reg >> 24) & 0xFF;
    d->subclass    = (class_reg >> 16) & 0xFF;
    d->prog_if     = (class_reg >> 8) & 0xFF;

    uint32_t hdr = pci_config_read32(bus, dev, func, 0x0C);
    d->header_type = (hdr >> 16) & 0xFF;

    /* Read BARs (only for header type 0) */
    if ((d->header_type & 0x7F) == 0) {
        for (int i = 0; i < 6; i++) {
            d->bar[i] = pci_config_read32(bus, dev, func, 0x10 + i * 4);
        }
    } else {
        for (int i = 0; i < 6; i++)
            d->bar[i] = 0;
    }

    device_count++;
}

int pci_enumerate(void)
{
    device_count = 0;

    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id = pci_config_read32(bus, dev, 0, 0x00);
            uint16_t vendor = id & 0xFFFF;

            if (vendor == 0xFFFF)
                continue;

            pci_scan_function(bus, dev, 0);

            /* Check for multi-function device */
            uint32_t hdr = pci_config_read32(bus, dev, 0, 0x0C);
            if ((hdr >> 16) & 0x80) {
                for (int func = 1; func < 8; func++) {
                    pci_scan_function(bus, dev, func);
                }
            }
        }
    }

    return device_count;
}

struct pci_device *pci_get_device(int index)
{
    if (index < 0 || index >= device_count)
        return 0;
    return &devices[index];
}

int pci_device_count(void)
{
    return device_count;
}

const char *pci_class_name(uint8_t class_code, uint8_t subclass)
{
    switch (class_code) {
    case 0x00: return "Unclassified";
    case 0x01:
        switch (subclass) {
        case 0x01: return "IDE Controller";
        case 0x06: return "SATA Controller";
        case 0x08: return "NVMe Controller";
        default:   return "Storage";
        }
    case 0x02:
        switch (subclass) {
        case 0x00: return "Ethernet";
        case 0x80: return "Network";
        default:   return "Network";
        }
    case 0x03:
        switch (subclass) {
        case 0x00: return "VGA Controller";
        case 0x02: return "3D Controller";
        default:   return "Display";
        }
    case 0x04: return "Multimedia";
    case 0x05: return "Memory Controller";
    case 0x06:
        switch (subclass) {
        case 0x00: return "Host Bridge";
        case 0x01: return "ISA Bridge";
        case 0x04: return "PCI-PCI Bridge";
        default:   return "Bridge";
        }
    case 0x07: return "Serial Controller";
    case 0x08: return "System Peripheral";
    case 0x09: return "Input Device";
    case 0x0C:
        switch (subclass) {
        case 0x03: return "USB Controller";
        case 0x05: return "SMBus Controller";
        default:   return "Serial Bus";
        }
    case 0x0D: return "Wireless";
    case 0x0E: return "Intelligent I/O";
    case 0x0F: return "Satellite";
    case 0x10: return "Encryption";
    case 0x11: return "Signal Processing";
    case 0x12: return "AI Accelerator";
    case 0x40: return "Co-processor";
    default:   return "Unknown";
    }
}

int ecam_available_check(void)
{
    return ecam_available;
}
