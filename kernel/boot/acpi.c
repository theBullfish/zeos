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
static acpi_fadt_info_t g_fadt;
static int g_initialized = 0;

/* Forward decls — bodies are below acpi_madt() accessor. */
typedef struct sdt_header_s sdt_header_t_fwd;
static void parse_fadt_from_table(sdt_header_t *t);
static void try_decode_s3(void);
static int  aml_find_s3(const uint8_t *aml, uint32_t len,
                        uint8_t *typa, uint8_t *typb);

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

    int got_madt = 0;
    for (int i = 0; i < n; i++) {
        uint64_t phys;
        if (use_xsdt) phys = ((uint64_t *)entries)[i];
        else          phys = ((uint32_t *)entries)[i];

        sdt_header_t *t = map_sdt(phys);
        if (!t) continue;
        if (sig_eq(t->signature, "APIC", 4)) {
            parse_madt((madt_header_t *)t);
            got_madt = 1;
        } else if (sig_eq(t->signature, "FACP", 4)) {
            parse_fadt_from_table(t);
        }
    }

    /* Decode _S3 from DSDT (only meaningful if FADT was present). */
    try_decode_s3();

    /* Fallback: scan SSDTs for _S3 if DSDT didn't have it. */
    if (g_fadt.valid && !g_fadt.slp_typ_valid) {
        for (int i = 0; i < n; i++) {
            uint64_t phys;
            if (use_xsdt) phys = ((uint64_t *)entries)[i];
            else          phys = ((uint32_t *)entries)[i];
            sdt_header_t *t = map_sdt(phys);
            if (!t) continue;
            if (!sig_eq(t->signature, "SSDT", 4)) continue;
            uint32_t aml_len = t->length - sizeof(sdt_header_t);
            const uint8_t *aml = (const uint8_t *)(t + 1);
            uint8_t a, b;
            if (aml_find_s3(aml, aml_len, &a, &b) == 0) {
                g_fadt.slp_typ_valid = 1;
                g_fadt.slp_typa = a;
                g_fadt.slp_typb = b;
                break;
            }
        }
    }

    g_initialized = 1;
    return got_madt ? 0 : -4;
}

acpi_madt_t *acpi_madt(void) { return &g_madt; }

/* ── FADT / FACS / minimal AML walker for _S3 ─────────────────────── */

#pragma pack(push, 1)
typedef struct {
    uint8_t  address_space_id;
    uint8_t  register_bit_width;
    uint8_t  register_bit_offset;
    uint8_t  access_size;
    uint64_t address;
} gas_t;

typedef struct {
    sdt_header_t hdr;
    uint32_t firmware_ctrl;     /* FACS phys (32-bit) */
    uint32_t dsdt;              /* DSDT phys (32-bit) */
    uint8_t  reserved_a;
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_blk_len;
    uint8_t  gpe1_blk_len;
    uint8_t  gpe1_base;
    uint8_t  cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alrm;
    uint8_t  mon_alrm;
    uint8_t  century;
    uint16_t iapc_boot_arch;
    uint8_t  reserved_b;
    uint32_t flags;
    gas_t    reset_reg;
    uint8_t  reset_value;
    uint16_t arm_boot_arch;
    uint8_t  fadt_minor_version;
    uint64_t x_firmware_ctrl;   /* 64-bit FACS phys */
    uint64_t x_dsdt;            /* 64-bit DSDT phys */
    gas_t    x_pm1a_evt_blk;
    gas_t    x_pm1b_evt_blk;
    gas_t    x_pm1a_cnt_blk;
    gas_t    x_pm1b_cnt_blk;
    /* trailing fields omitted — Zeos only needs through x_pm1b_cnt_blk */
} fadt_t;

typedef struct {
    char     signature[4];      /* "FACS" */
    uint32_t length;
    uint32_t hardware_signature;
    uint32_t firmware_waking_vector;  /* 32-bit wake vector */
    uint32_t global_lock;
    uint32_t flags;
    uint64_t x_firmware_waking_vector;/* 64-bit wake vector */
    uint8_t  version;
    uint8_t  reserved[3];
    uint32_t ospm_flags;
    uint8_t  reserved2[24];
} facs_t;
#pragma pack(pop)

/* ── Tiny AML walker: scan DSDT bytes for "_S3_" name + Package. ───
 *
 * AML for `Name (_S3_, Package () {byte_a, byte_b, 0, 0})` looks like:
 *    08 5F 53 33 5F 12 <pkglen> 04 0A <byte_a> 0A <byte_b> 00 00
 * where:
 *    08            = NameOp
 *    5F 53 33 5F   = "_S3_"
 *    12            = PackageOp
 *    <pkglen>      = 1-4 byte package length
 *    04            = NumElements (4)
 *    0A <b>        = ByteOp + value (or 00/01 for 0/1)
 *
 * We don't parse pkglen properly; we just scan the raw byte stream
 * for the "08 5F 53 33 5F 12" prefix and grab the next two byte
 * values, accepting either 0x0A-prefixed bytes or single-byte 0/1
 * constants. This is sufficient for every firmware that exposes _S3.
 *
 * If _S3 isn't in the DSDT (some firmware puts it in an SSDT), we
 * also fall back to scanning all loaded SSDTs after this. */
static int aml_decode_byte(const uint8_t *p, const uint8_t *end,
                           uint8_t *out, int *consumed)
{
    if (p >= end) return -1;
    if (*p == 0x00) { *out = 0; *consumed = 1; return 0; }
    if (*p == 0x01) { *out = 1; *consumed = 1; return 0; }
    if (*p == 0x0A) {
        if (p + 1 >= end) return -1;
        *out = p[1]; *consumed = 2; return 0;
    }
    /* Word/Dword/QWord constants are unusual for _Sx values but skip
     * them safely. */
    if (*p == 0x0B) { if (p+2>=end) return -1; *out = p[1]; *consumed = 3; return 0; }
    if (*p == 0x0C) { if (p+4>=end) return -1; *out = p[1]; *consumed = 5; return 0; }
    return -1;
}

static int aml_find_s3(const uint8_t *aml, uint32_t len,
                       uint8_t *typa, uint8_t *typb)
{
    if (len < 16) return -1;
    const uint8_t pat[6] = {0x08, 0x5F, 0x53, 0x33, 0x5F, 0x12};
    for (uint32_t i = 0; i + 8 < len; i++) {
        int match = 1;
        for (int j = 0; j < 6; j++) {
            if (aml[i+j] != pat[j]) { match = 0; break; }
        }
        if (!match) continue;
        /* aml[i+6] = pkglen byte 0. Top 2 bits = number of follow-up
         * pkglen bytes (0..3). Skip them. */
        uint32_t k = i + 6;
        uint8_t pkg0 = aml[k++];
        int extra = (pkg0 >> 6) & 0x3;
        k += extra;
        if (k >= len) return -1;
        /* NumElements byte */
        if (aml[k] == 0x04 || aml[k] == 0x03 || aml[k] == 0x02) k++;
        else { /* tolerate firmware that omits or uses different shape */ }
        uint8_t a = 0, b = 0;
        int c = 0;
        if (k >= len) return -1;
        if (aml_decode_byte(aml + k, aml + len, &a, &c) != 0) return -1;
        k += c;
        if (k >= len) { *typa = a; *typb = a; return 0; }
        if (aml_decode_byte(aml + k, aml + len, &b, &c) != 0) {
            *typa = a; *typb = a; return 0;
        }
        *typa = a;
        *typb = b;
        return 0;
    }
    return -1;
}

static void parse_fadt(fadt_t *f)
{
    g_fadt.valid        = 1;
    g_fadt.pm1a_cnt_blk = (uint16_t)f->pm1a_cnt_blk;
    g_fadt.pm1b_cnt_blk = (uint16_t)f->pm1b_cnt_blk;
    g_fadt.pm1a_evt_blk = (uint16_t)f->pm1a_evt_blk;
    g_fadt.pm1b_evt_blk = (uint16_t)f->pm1b_evt_blk;
    g_fadt.pm1_evt_len  = f->pm1_evt_len;
    g_fadt.pm1_cnt_len  = f->pm1_cnt_len;
    g_fadt.facs_addr    = f->firmware_ctrl;
    g_fadt.dsdt_addr    = f->dsdt;
    g_fadt.smi_cmd      = (uint16_t)f->smi_cmd;
    g_fadt.acpi_enable  = f->acpi_enable;
    g_fadt.acpi_disable = f->acpi_disable;

    /* Prefer 64-bit X_ fields when the FADT is long enough to contain
     * them and they are non-zero. */
    if (f->hdr.length >= sizeof(fadt_t)) {
        if (f->x_firmware_ctrl) g_fadt.x_facs_addr = f->x_firmware_ctrl;
        if (f->x_dsdt)          g_fadt.x_dsdt_addr = f->x_dsdt;
        if (f->x_pm1a_cnt_blk.address)
            g_fadt.pm1a_cnt_blk = (uint16_t)f->x_pm1a_cnt_blk.address;
        if (f->x_pm1b_cnt_blk.address)
            g_fadt.pm1b_cnt_blk = (uint16_t)f->x_pm1b_cnt_blk.address;
    }
}

static void try_decode_s3(void)
{
    if (!g_fadt.valid) return;
    uint64_t dsdt_phys = g_fadt.x_dsdt_addr ? g_fadt.x_dsdt_addr
                                            : (uint64_t)g_fadt.dsdt_addr;
    if (!dsdt_phys) return;

    sdt_header_t *dsdt = map_sdt(dsdt_phys);
    if (!dsdt) return;
    if (!sig_eq(dsdt->signature, "DSDT", 4)) return;

    uint32_t aml_len = dsdt->length - sizeof(sdt_header_t);
    const uint8_t *aml = (const uint8_t *)(dsdt + 1);

    uint8_t a, b;
    if (aml_find_s3(aml, aml_len, &a, &b) == 0) {
        g_fadt.slp_typ_valid = 1;
        g_fadt.slp_typa = a;
        g_fadt.slp_typb = b;
    }
}

const acpi_fadt_info_t *acpi_fadt(void) { return &g_fadt; }

int acpi_set_wake_vector(uint32_t wake_paddr)
{
    if (!g_fadt.valid) return -1;
    uint64_t facs_phys = g_fadt.x_facs_addr ? g_fadt.x_facs_addr
                                             : (uint64_t)g_fadt.facs_addr;
    if (!facs_phys) return -2;
    facs_t *facs = (facs_t *)map_phys(facs_phys, sizeof(facs_t));
    if (!facs) return -3;
    if (!sig_eq(facs->signature, "FACS", 4)) return -4;
    facs->firmware_waking_vector = wake_paddr;
    /* Per ACPI spec, when waking in 64-bit mode the X field can be
     * used; when zero the firmware uses the 32-bit field. We zero X
     * to force the firmware down the well-tested 16-bit real-mode
     * path, which our wake stub is written for. */
    facs->x_firmware_waking_vector = 0;
    return 0;
}

/* Hook into acpi_init to also look for FADT.
 * Called from inside the table walk (see patch in acpi_init). */
static void parse_fadt_from_table(sdt_header_t *t)
{
    if (sig_eq(t->signature, "FACP", 4)) {
        parse_fadt((fadt_t *)t);
    }
}

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
