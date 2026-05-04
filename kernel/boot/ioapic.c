/*
 * Zeos — IOAPIC.
 *
 * Minimal but real: maps every IOAPIC ACPI told us about, exposes
 * its max redirection-entry count, and provides ioapic_set_irq()
 * which honors MADT Interrupt Source Override entries (legacy ISA
 * IRQ -> Global System Interrupt remapping, polarity, trigger).
 *
 * Currently no Zeos subsystem requires legacy-IRQ delivery via the
 * APIC path — the PIT IRQ0 still flows through the 8259, and MSI-X
 * is direct LAPIC. This driver is here so the next agent can switch
 * legacy IRQs over without revisiting the topology code.
 */

#include "ioapic.h"
#include "acpi.h"
#include "vmm.h"
#include "kprint.h"

#define IOAPIC_REGSEL 0x00
#define IOAPIC_WIN    0x10

#define IOAPIC_REG_ID    0x00
#define IOAPIC_REG_VER   0x01
#define IOAPIC_REG_RED0  0x10  /* low half of redirection entry 0 */

#define IOAPIC_RED_MASKED  (1u << 16)
#define IOAPIC_RED_LEVEL   (1u << 15)
#define IOAPIC_RED_LOWPOL  (1u << 13)

#define MAX_IOAPIC 8

typedef struct {
    volatile uint8_t *mmio;
    uint32_t gsi_base;
    int      max_redir;  /* number of redirection entries - 1 */
} ioapic_inst_t;

static ioapic_inst_t g_io[MAX_IOAPIC];
static int g_count = 0;

static uint32_t io_read(ioapic_inst_t *io, uint8_t reg)
{
    *(volatile uint32_t *)(io->mmio + IOAPIC_REGSEL) = reg;
    return *(volatile uint32_t *)(io->mmio + IOAPIC_WIN);
}

static void io_write(ioapic_inst_t *io, uint8_t reg, uint32_t val)
{
    *(volatile uint32_t *)(io->mmio + IOAPIC_REGSEL) = reg;
    *(volatile uint32_t *)(io->mmio + IOAPIC_WIN)    = val;
}

static ioapic_inst_t *find_for_gsi(uint32_t gsi)
{
    for (int i = 0; i < g_count; i++) {
        if (gsi >= g_io[i].gsi_base &&
            gsi <= g_io[i].gsi_base + (uint32_t)g_io[i].max_redir)
            return &g_io[i];
    }
    return 0;
}

int ioapic_init(void)
{
    g_count = 0;
    acpi_madt_t *m = acpi_madt();
    if (!m || m->ioapic_count == 0) return -1;

    int n = m->ioapic_count;
    if (n > MAX_IOAPIC) n = MAX_IOAPIC;

    for (int i = 0; i < n; i++) {
        acpi_ioapic_t *src = acpi_ioapic(i);
        if (!src) continue;
        uint64_t base = src->addr;
        vmm_map_range(base & ~0xFFFULL, base & ~0xFFFULL, 1,
                      PTE_WRITABLE | PTE_NOCACHE);
        ioapic_inst_t *io = &g_io[g_count++];
        io->mmio = (volatile uint8_t *)(uintptr_t)base;
        io->gsi_base = src->gsi_base;
        uint32_t ver = io_read(io, IOAPIC_REG_VER);
        io->max_redir = (int)((ver >> 16) & 0xFF);

        /* Mask every redirection entry; the rest of the kernel will
         * unmask the ones it cares about. */
        for (int e = 0; e <= io->max_redir; e++) {
            io_write(io, IOAPIC_REG_RED0 + e * 2,     IOAPIC_RED_MASKED);
            io_write(io, IOAPIC_REG_RED0 + e * 2 + 1, 0);
        }
    }
    return 0;
}

int ioapic_count(void) { return g_count; }

int ioapic_max_redirections(int idx)
{
    if (idx < 0 || idx >= g_count) return 0;
    return g_io[idx].max_redir + 1;
}

void ioapic_set_irq(uint8_t legacy_irq, uint8_t vector, uint8_t lapic_id)
{
    /* Resolve legacy ISA IRQ -> GSI via Interrupt Source Override. */
    uint32_t gsi = legacy_irq;
    uint32_t flags = 0;
    const acpi_iso_t *iso = acpi_iso_for(legacy_irq);
    if (iso) {
        gsi = iso->gsi;
        flags = iso->flags;
    }

    ioapic_inst_t *io = find_for_gsi(gsi);
    if (!io) return;

    uint32_t entry_idx = gsi - io->gsi_base;
    uint32_t low = vector;

    /* Polarity: bits 0-1 of MADT flags. 11 = active low. */
    if ((flags & 0x3) == 0x3) low |= IOAPIC_RED_LOWPOL;
    /* Trigger: bits 2-3. 11 = level. */
    if ((flags & 0xC) == 0xC) low |= IOAPIC_RED_LEVEL;

    uint32_t high = ((uint32_t)lapic_id) << 24;

    io_write(io, IOAPIC_REG_RED0 + entry_idx * 2 + 1, high);
    io_write(io, IOAPIC_REG_RED0 + entry_idx * 2,     low);
}

void ioapic_mask(uint8_t legacy_irq)
{
    uint32_t gsi = legacy_irq;
    const acpi_iso_t *iso = acpi_iso_for(legacy_irq);
    if (iso) gsi = iso->gsi;
    ioapic_inst_t *io = find_for_gsi(gsi);
    if (!io) return;
    uint32_t entry_idx = gsi - io->gsi_base;
    uint32_t cur = io_read(io, IOAPIC_REG_RED0 + entry_idx * 2);
    io_write(io, IOAPIC_REG_RED0 + entry_idx * 2, cur | IOAPIC_RED_MASKED);
}
