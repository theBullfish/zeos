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
#include "hwdb.h"
#include "io.h"
#include "hal.h"   /* O.2: PCI config I/O via the HAL */
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

/* outl/inl now provided by io.h */
static uint32_t pci_legacy_read32(uint8_t bus, uint8_t dev, uint8_t func,
                                   uint8_t offset)
{
    uint32_t addr = (1U << 31)                /* Enable bit */
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8)
                  | (offset & 0xFC);

    hal_out32(PCI_CONFIG_ADDR, addr);
    return hal_in32(PCI_CONFIG_DATA);
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

void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val)
{
    if (ecam_available) {
        uint64_t addr = ecam_base
                      + ((uint64_t)bus << 20)
                      + ((uint64_t)dev << 15)
                      + ((uint64_t)func << 12)
                      + offset;
        *(volatile uint32_t *)addr = val;
    } else {
        uint32_t addr = (1U << 31)
                      | ((uint32_t)bus << 16)
                      | ((uint32_t)dev << 11)
                      | ((uint32_t)func << 8)
                      | (offset & 0xFC);
        hal_out32(PCI_CONFIG_ADDR, addr);
        hal_out32(PCI_CONFIG_DATA, val);
    }
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

/* ─── PCIe capability walking ──────────────────────────── */

uint8_t pci_find_capability(struct pci_device *dev, uint8_t cap_id)
{
    if (!dev) return 0;
    /* Status register bit 4 (Capabilities List) must be set. */
    uint32_t status_cmd = pci_config_read32(dev->bus, dev->dev, dev->func, 0x04);
    if (!((status_cmd >> 16) & (1u << 4))) return 0;

    /* Capabilities Pointer at offset 0x34 (header type 0) or 0x14 (type 1).
     * Top 2 bits of header_type encode multifunction; lower 7 bits the type. */
    uint8_t htype = dev->header_type & 0x7f;
    uint8_t cap_off_reg = (htype == 0x01 || htype == 0x02) ? 0x14 : 0x34;
    uint32_t pp = pci_config_read32(dev->bus, dev->dev, dev->func, cap_off_reg);
    uint8_t off = (uint8_t)(pp & 0xfc);  /* low 2 bits reserved */

    int hops = 0;
    while (off && off != 0xff && hops < 48) {
        uint32_t cap = pci_config_read32(dev->bus, dev->dev, dev->func, off);
        uint8_t this_id = (uint8_t)(cap & 0xff);
        if (this_id == cap_id) return off;
        off = (uint8_t)((cap >> 8) & 0xfc);
        hops++;
    }
    return 0;
}

int pci_has_msi(struct pci_device *dev)  { return pci_find_capability(dev, 0x05) != 0; }
int pci_has_msix(struct pci_device *dev) { return pci_find_capability(dev, 0x11) != 0; }

int pci_link_speed(struct pci_device *dev)
{
    uint8_t pcie = pci_find_capability(dev, 0x10);
    if (!pcie) return 0;
    /* Link Status register at cap_offset + 0x12 (16 bits) */
    uint32_t lc = pci_config_read32(dev->bus, dev->dev, dev->func, pcie + 0x10);
    uint32_t lstatus = (lc >> 16) & 0xffff;
    return (int)(lstatus & 0xf);
}

int pci_link_width(struct pci_device *dev)
{
    uint8_t pcie = pci_find_capability(dev, 0x10);
    if (!pcie) return 0;
    uint32_t lc = pci_config_read32(dev->bus, dev->dev, dev->func, pcie + 0x10);
    uint32_t lstatus = (lc >> 16) & 0xffff;
    return (int)((lstatus >> 4) & 0x3f);
}

const char *pci_link_speed_name(int speed_code)
{
    switch (speed_code) {
        case 1: return "Gen1 (2.5 GT/s)";
        case 2: return "Gen2 (5 GT/s)";
        case 3: return "Gen3 (8 GT/s)";
        case 4: return "Gen4 (16 GT/s)";
        case 5: return "Gen5 (32 GT/s)";
        case 6: return "Gen6 (64 GT/s)";
        default: return "?";
    }
}

uint64_t pci_bar_size(struct pci_device *dev, int bar_idx)
{
    if (!dev || bar_idx < 0 || bar_idx > 5) return 0;
    uint8_t off = (uint8_t)(0x10 + bar_idx * 4);

    /* Save the original BAR. */
    uint32_t orig = pci_config_read32(dev->bus, dev->dev, dev->func, off);
    if (orig == 0) return 0;

    /* Write all-ones, read back the size mask, restore. */
    pci_config_write32(dev->bus, dev->dev, dev->func, off, 0xffffffffu);
    uint32_t sz_lo = pci_config_read32(dev->bus, dev->dev, dev->func, off);
    pci_config_write32(dev->bus, dev->dev, dev->func, off, orig);

    /* I/O BAR: bit 0 set, mask is ~3. Mem BAR: bit 0 clear, mask is ~F. */
    uint32_t mask = (orig & 1u) ? (sz_lo & ~0x3u) : (sz_lo & ~0xfu);
    if (mask == 0) return 0;

    /* For 64-bit memory BARs (type bits [2:1] == 10), include the upper. */
    uint32_t high_mask = 0;
    int is_64 = !(orig & 1u) && ((orig & 0x6u) == 0x4u);
    if (is_64 && bar_idx < 5) {
        uint32_t high_off = (uint8_t)(off + 4);
        uint32_t hi_orig = pci_config_read32(dev->bus, dev->dev, dev->func, high_off);
        pci_config_write32(dev->bus, dev->dev, dev->func, high_off, 0xffffffffu);
        high_mask = pci_config_read32(dev->bus, dev->dev, dev->func, high_off);
        pci_config_write32(dev->bus, dev->dev, dev->func, high_off, hi_orig);
    }

    /* Size is one more than the inverse of the writable bits. */
    uint64_t size_mask = ((uint64_t)high_mask << 32) | mask;
    /* sign-extend low 32 bits if no high portion */
    if (!is_64) size_mask = (uint64_t)mask | 0xffffffff00000000ull;
    return (~size_mask) + 1;
}

/* ─── Vendor / device name database (best effort) ─────── */

const char *pci_vendor_name(uint16_t vid)
{
    switch (vid) {
        case 0x8086: return "Intel";
        case 0x1022: return "AMD";
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "AMD/ATI";
        case 0x10EC: return "Realtek";
        case 0x14E4: return "Broadcom";
        case 0x168C: return "Qualcomm Atheros";
        case 0x10DF: return "Emulex";
        case 0x15B3: return "Mellanox";
        case 0x144D: return "Samsung";
        case 0x1B36: return "Red Hat (virtio)";
        case 0x1AF4: return "Red Hat (virtio)";
        case 0x1234: return "QEMU/Bochs VGA";
        case 0x1DA3: return "Habana Labs";
        case 0x10A9: return "Solarflare";
        case 0x12D8: return "Pericom";
        case 0x10B5: return "PLX/Avago bridge";
        case 0x1077: return "QLogic";
        case 0x14C1: return "Myricom";
        case 0x18A1: return "Xilinx";
        case 0x10EE: return "Xilinx (alt)";
        case 0x1957: return "Freescale/NXP";
        case 0x12B7: return "Cirrus Logic";
        default:     break;
    }
    /* Not in the curated list: consult the preloaded hardware knowledge base
     * (2378 vendors from pci.ids, compiled in — no network, no lookup service). */
    { const char *n = hwdb_pci_vendor(vid); if (n) return n; }
    return "Unknown";
}

const char *pci_device_name(uint16_t vid, uint16_t did)
{
    if (vid == 0x8086) {
        switch (did) {
            case 0x100E: return "82540EM Gigabit (e1000)";
            case 0x10D3: return "82574L Gigabit";
            case 0x153A: return "I217-LM";
            case 0x15A0: return "I218-LM";
            case 0x15A3: return "I219-LM";
            case 0x1570: return "I219-V";
            case 0x9D2F: return "Sunrise Point USB 3.0";
            case 0x15D4: return "Alpine Ridge Thunderbolt 3";
            case 0x15EB: return "Titan Ridge Thunderbolt 3";
            case 0x1137: return "Maple Ridge Thunderbolt 4";
            case 0x9DED: return "Sunrise Point AHCI";
            case 0x2922: return "ICH9 AHCI";
            default: break;
        }
    } else if (vid == 0x10EC) {
        switch (did) {
            case 0x8139: return "RTL8139 Fast Ethernet";
            case 0x8169: return "RTL8169 Gigabit";
            case 0x8168: return "RTL8168 Gigabit";
            case 0x8161: return "RTL8161 Gigabit";
            case 0x8136: return "RTL8101E Fast Ethernet";
            default: break;
        }
    } else if (vid == 0x1AF4) {
        switch (did) {
            case 0x1000: return "virtio-net (legacy)";
            case 0x1001: return "virtio-block (legacy)";
            case 0x1041: return "virtio-net";
            case 0x1042: return "virtio-block";
            default: break;
        }
    } else if (vid == 0x1DA3) {
        if (did == 0x0001) return "Goya HL-1000";
    }
    /* 18685 device names from the preloaded knowledge base. */
    { const char *n = hwdb_pci_device(vid, did); if (n) return n; }
    return "";
}
