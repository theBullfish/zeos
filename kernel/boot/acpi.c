/*
 * Zeos — ACPI parser.
 *
 * Walks RSDP -> XSDT (or RSDT fallback) -> finds the MADT (sig "APIC"),
 * captures LAPIC base + flags, and parses entry types we care about:
 *   0  Processor LAPIC
 *   1  IOAPIC
 *   2  Interrupt Source Override
 *   4  Local APIC NMI
 *   5  Local APIC Address Override (updates lapic_addr)
 * Everything else is skipped via the entry length field.
 */

#include "acpi.h"
#include "vmm.h"
#include "kprint.h"

#define ACPI_MADT_LAPIC          0
#define ACPI_MADT_IOAPIC         1
#define ACPI_MADT_ISO            2
#define ACPI_MADT_NMI_SOURCE     3
#define ACPI_MADT_LAPIC_NMI      4
#define ACPI_MADT_LAPIC_ADDR_OVR 5

#pragma pack(push, 1)
typedef struct {
    char     signature[8];   /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    /* ACPI 2.0+ fields below: only valid if revision >= 2 */
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} rsdp_t;

typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} sdt_header_t;

typedef struct {
    sdt_header_t hdr;
    uint32_t lapic_addr;
    uint32_t flags;
    uint8_t  entries[];
} madt_header_t;

typedef struct {
    uint8_t type;
    uint8_t length;
} madt_entry_t;
#pragma pack(pop)

static acpi_madt_t g_madt;
static int g_initialized = 0;

static int sig_eq(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Identity-map a physical region so we can read it as a kernel pointer. */
static void *map_phys(uint64_t phys, uint64_t bytes)
{
    uint64_t base = phys & ~0xFFFULL;
    uint64_t end  = (phys + bytes + 0xFFF) & ~0xFFFULL;
    uint64_t pages = (end - base) / 0x1000;
    if (pages == 0) pages = 1;
    vmm_map_range(base, base, pages, PTE_WRITABLE);
    return (void *)(uintptr_t)phys;
}

static sdt_header_t *map_sdt(uint64_t phys)
{
    if (!phys) return 0;
    sdt_header_t *h = (sdt_header_t *)map_phys(phys, sizeof(sdt_header_t));
    if (!h) return 0;
    /* Re-map with full length now that we know it. */
    return (sdt_header_t *)map_phys(phys, h->length);
}

static void parse_madt(madt_header_t *m)
{
    g_madt.lapic_addr = m->lapic_addr;
    g_madt.flags      = m->flags;

    uint8_t *p   = m->entries;
    uint8_t *end = (uint8_t *)m + m->hdr.length;

    while (p + sizeof(madt_entry_t) <= end) {
        madt_entry_t *e = (madt_entry_t *)p;
        if (e->length == 0) break;

        switch (e->type) {
        case ACPI_MADT_LAPIC:
            if (g_madt.lapic_count < ACPI_MAX_LAPICS && e->length >= 8) {
                acpi_lapic_t *l = &g_madt.lapics[g_madt.lapic_count++];
                l->acpi_proc_id = p[2];
                l->apic_id      = p[3];
                l->flags        = *(uint32_t *)(p + 4);
            }
            break;
        case ACPI_MADT_IOAPIC:
            if (g_madt.ioapic_count < ACPI_MAX_IOAPICS && e->length >= 12) {
                acpi_ioapic_t *io = &g_madt.ioapics[g_madt.ioapic_count++];
                io->id       = p[2];
                /* p[3] is reserved */
                io->addr     = *(uint32_t *)(p + 4);
                io->gsi_base = *(uint32_t *)(p + 8);
            }
            break;
        case ACPI_MADT_ISO:
            if (g_madt.iso_count < ACPI_MAX_ISOS && e->length >= 10) {
                acpi_iso_t *iso = &g_madt.isos[g_madt.iso_count++];
                iso->bus        = p[2];
                iso->source_irq = p[3];
                iso->gsi        = *(uint32_t *)(p + 4);
                iso->flags      = *(uint16_t *)(p + 8);
            }
            break;
        case ACPI_MADT_LAPIC_NMI:
            if (g_madt.nmi_count < ACPI_MAX_NMIS && e->length >= 6) {
                acpi_nmi_t *n = &g_madt.nmis[g_madt.nmi_count++];
                n->acpi_proc_id = p[2];
                n->flags        = *(uint16_t *)(p + 3);
                n->lint         = p[5];
            }
            break;
        case ACPI_MADT_LAPIC_ADDR_OVR:
            if (e->length >= 12) {
                g_madt.lapic_addr = *(uint64_t *)(p + 4);
            }
            break;
        default:
            break;
        }
        p += e->length;
    }
}

int acpi_init(void *rsdp_ptr)
{
    if (g_initialized) return 0;
    if (!rsdp_ptr) return -1;

    /* Zero our tables. */
    g_madt.lapic_addr   = 0xFEE00000ULL;  /* sane default */
    g_madt.flags        = 0;
    g_madt.lapic_count  = 0;
    g_madt.ioapic_count = 0;
    g_madt.iso_count    = 0;
    g_madt.nmi_count    = 0;

    /* RSDP is in UEFI Configuration Table memory, already mapped, but
     * map defensively in case the firmware placed it in a region we
     * have not touched yet. */
    map_phys((uint64_t)(uintptr_t)rsdp_ptr, sizeof(rsdp_t));
    rsdp_t *rsdp = (rsdp_t *)rsdp_ptr;

    if (!sig_eq(rsdp->signature, "RSD PTR ", 8)) return -2;

    sdt_header_t *root = 0;
    int use_xsdt = 0;
    if (rsdp->revision >= 2 && rsdp->xsdt_addr) {
        root = map_sdt(rsdp->xsdt_addr);
        use_xsdt = 1;
    }
    if (!root && rsdp->rsdt_addr) {
        root = map_sdt((uint64_t)rsdp->rsdt_addr);
        use_xsdt = 0;
    }
    if (!root) return -3;

    int n = (root->length - sizeof(sdt_header_t)) /
            (use_xsdt ? 8 : 4);
    uint8_t *entries = (uint8_t *)(root + 1);

    for (int i = 0; i < n; i++) {
        uint64_t phys;
        if (use_xsdt) phys = ((uint64_t *)entries)[i];
        else          phys = ((uint32_t *)entries)[i];

        sdt_header_t *t = map_sdt(phys);
        if (!t) continue;
        if (sig_eq(t->signature, "APIC", 4)) {
            parse_madt((madt_header_t *)t);
            g_initialized = 1;
            return 0;
        }
    }
    /* No MADT — leave defaults; init still useful for downstream callers. */
    g_initialized = 1;
    return -4;
}

acpi_madt_t *acpi_madt(void) { return &g_madt; }

acpi_lapic_t *acpi_lapic_for(int cpu)
{
    if (cpu < 0 || cpu >= g_madt.lapic_count) return 0;
    return &g_madt.lapics[cpu];
}

acpi_ioapic_t *acpi_ioapic(int idx)
{
    if (idx < 0 || idx >= g_madt.ioapic_count) return 0;
    return &g_madt.ioapics[idx];
}

const acpi_iso_t *acpi_iso_for(uint8_t legacy_irq)
{
    for (int i = 0; i < g_madt.iso_count; i++) {
        if (g_madt.isos[i].source_irq == legacy_irq) return &g_madt.isos[i];
    }
    return 0;
}
