/*
 * Zeos — ACPI table parsing.
 *
 * Pulls RSDP from the UEFI System Table (passed in boot info), walks
 * XSDT (or RSDT) and surfaces parsed MADT entries (Processor LAPIC,
 * IOAPIC, Interrupt Source Override, Local APIC NMI).
 *
 * This is intentionally minimal: only the structures Zeos needs to
 * bring up the LAPIC and the first IOAPIC. Everything is kept in
 * static storage; no allocations, no parsing of optional tables.
 */

#ifndef ZEOS_ACPI_H
#define ZEOS_ACPI_H

#include <stdint.h>

#define ACPI_MAX_LAPICS  64
#define ACPI_MAX_IOAPICS 8
#define ACPI_MAX_ISOS    32
#define ACPI_MAX_NMIS    8

typedef struct {
    uint8_t  acpi_proc_id;
    uint8_t  apic_id;
    uint32_t flags;       /* bit 0 = enabled, bit 1 = online-capable */
} acpi_lapic_t;

typedef struct {
    uint8_t  id;
    uint32_t addr;        /* IOAPIC MMIO physical base */
    uint32_t gsi_base;    /* Global System Interrupt base */
} acpi_ioapic_t;

typedef struct {
    uint8_t  bus;
    uint8_t  source_irq;  /* legacy ISA IRQ */
    uint32_t gsi;         /* mapped Global System Interrupt */
    uint16_t flags;       /* polarity (bits 0-1) + trigger (bits 2-3) */
} acpi_iso_t;

typedef struct {
    uint8_t  acpi_proc_id;  /* 0xFF == all processors */
    uint16_t flags;
    uint8_t  lint;          /* LINT0 / LINT1 */
} acpi_nmi_t;

typedef struct {
    uint64_t lapic_addr;    /* Local APIC MMIO base from MADT (or override) */
    uint32_t flags;         /* MADT flags (bit 0: PCAT_COMPAT) */
    int      lapic_count;
    int      ioapic_count;
    int      iso_count;
    int      nmi_count;
    acpi_lapic_t  lapics[ACPI_MAX_LAPICS];
    acpi_ioapic_t ioapics[ACPI_MAX_IOAPICS];
    acpi_iso_t    isos[ACPI_MAX_ISOS];
    acpi_nmi_t    nmis[ACPI_MAX_NMIS];
} acpi_madt_t;

int acpi_init(void *rsdp);
acpi_madt_t  *acpi_madt(void);
acpi_lapic_t *acpi_lapic_for(int cpu);
acpi_ioapic_t *acpi_ioapic(int idx);
const acpi_iso_t *acpi_iso_for(uint8_t legacy_irq);

#endif /* ZEOS_ACPI_H */
