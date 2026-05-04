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

/* ── FADT / FACS / DSDT _S3 — needed for ACPI S3 suspend ─────────── */

typedef struct {
    int      valid;            /* 1 if FADT was parsed */
    uint16_t pm1a_cnt_blk;     /* I/O port for PM1a control */
    uint16_t pm1b_cnt_blk;     /* I/O port for PM1b control (0 if not present) */
    uint16_t pm1a_evt_blk;     /* I/O port for PM1a event (status+enable) */
    uint16_t pm1b_evt_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint32_t facs_addr;        /* 32-bit FACS phys */
    uint64_t x_facs_addr;      /* 64-bit FACS phys (preferred if non-zero) */
    uint32_t dsdt_addr;        /* 32-bit DSDT phys */
    uint64_t x_dsdt_addr;      /* 64-bit DSDT phys (preferred) */
    uint8_t  smi_cmd_byte;     /* byte to write to SMI_CMD to enable ACPI mode */
    uint16_t smi_cmd;          /* SMI command port */
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;

    /* _S3 package values (decoded from DSDT). slp_typa/b are the values
     * to write into PM1a_CNT.SLP_TYP / PM1b_CNT.SLP_TYP (already shifted
     * into bits 10-12 of PM1_CNT). slp_typ_valid=1 if _S3 was located. */
    int      slp_typ_valid;
    uint8_t  slp_typa;
    uint8_t  slp_typb;
} acpi_fadt_info_t;

const acpi_fadt_info_t *acpi_fadt(void);

/* Set the firmware ACPI wake vector. Writes `wake_paddr` (must be
 * <1MB and 4-byte aligned for the 32-bit field; the 64-bit field
 * carries the same value) into FACS. Returns 0 on success. */
int acpi_set_wake_vector(uint32_t wake_paddr);

/* ── AML table iteration ────────────────────────────────────────────
 * Cached list of every AML-bearing table parsed at acpi_init time
 * (DSDT + every SSDT). Battery, EC, thermal etc. all need to scan
 * AML for named objects. We don't run a full AML interpreter — these
 * pointers are raw byte buffers callers pattern-scan. */
#define ACPI_MAX_AML_TABLES 16

typedef struct {
    const uint8_t *aml;     /* AML byte stream (after the SDT header) */
    uint32_t       len;     /* bytes of AML */
    char           sig[4];  /* "DSDT" or "SSDT" */
} acpi_aml_table_t;

int                     acpi_aml_table_count(void);
const acpi_aml_table_t *acpi_aml_table(int idx);

#endif /* ZEOS_ACPI_H */
