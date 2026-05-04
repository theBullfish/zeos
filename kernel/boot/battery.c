/*
 * Zeos — ACPI battery + AC detect.
 *
 * ACPI battery + AC detect via DSDT/SSDT scan. NO full AML evaluator —
 * pulls static _BST/_BIF/_PSR fields and gracefully reports UNKNOWN
 * for batteries whose values are computed by AML methods.
 * SBS (Smart Battery System / I2C) is a separate path.
 *
 * What we DO support:
 *   - Detect presence of a Control Method Battery (_HID PNP0C0A) and
 *     the AC adapter (_HID ACPI0003) by pattern-scanning AML for the
 *     EISA-id encoded byte sequences.
 *   - Extract `Name (_BIF, Package(...))` and `Name (_BST, Package(...))`
 *     when the firmware inlined them as Name objects with literal
 *     integer fields. Many older / simpler firmwares do this.
 *
 * What we DO NOT support:
 *   - Any value that is the return of a Method (`Method (_BST,...) {
 *     Return(...) }`). Modern EC-backed laptops put _BIF/_BST behind
 *     methods that read the EC over the LPC bus; those need a real
 *     interpreter (and an EC driver). We report present=1, values
 *     unknown, state=BAT_UNKNOWN.
 *   - Notification routing (Notify(BAT0,0x80)): polling only.
 *   - SBS / I2C smart batteries.
 *
 * Output of cmd_battery on a laptop with method-only _BST will look
 * like:
 *   AC: unknown
 *   Battery 0: present, charge unknown (no AML evaluator)
 *
 * That is the honest scope. When an evaluator lands the same module
 * fills in the numbers without a public-API change.
 */

#include "battery.h"
#include "acpi.h"
#include "aml.h"
#include "kprint.h"

/* ── Module state ──────────────────────────────────────────────── */

static battery_info_t g_bats[BATTERY_MAX];
static int            g_bat_count = 0;
static int            g_ac_present_known = 0;
static int            g_ac_online = 0;
static int            g_initialized = 0;

/* ── EISA-id helpers ──────────────────────────────────────────── */
/*
 * `_HID` in AML is encoded as a 4-byte DWordConst (op 0x0C) carrying
 * a packed EISA id. We compare against a known DWORD pattern.
 *
 *   PNP0C0A (Control Method Battery) -> 0x410CD041
 *   ACPI0003 (AC adapter)            -> 0x0300d041 (variant) /
 *                                       byte stream 41 D0 03 00
 *
 * The real encoding: take the 7-char EISA id "PNP0C0A", pack the
 * three letters (each 1..26) into a 16-bit BE word, append the
 * 4-hex-digit product code BE, and store the resulting 4 bytes
 * little-endian. We just hard-code the 4-byte literal patterns.
 */

static const uint8_t HID_BATTERY[4] = { 0x41, 0xD0, 0x0C, 0x0A };
static const uint8_t HID_AC[4]      = { 0x41, 0xD0, 0x00, 0x03 };

/* AML opcodes we touch */
#define AML_NAME_OP    0x08
#define AML_PACKAGE_OP 0x12
#define AML_BYTE_OP    0x0A
#define AML_WORD_OP    0x0B
#define AML_DWORD_OP   0x0C
#define AML_QWORD_OP   0x0E
#define AML_ZERO_OP    0x00
#define AML_ONE_OP     0x01
#define AML_ONES_OP    0xFF

/* ── AML decode helpers ────────────────────────────────────────── */

/* Decode an integer constant. Returns bytes consumed, 0 on no-match. */
static int decode_int(const uint8_t *p, const uint8_t *end, uint64_t *out)
{
    if (p >= end) return 0;
    if (*p == AML_ZERO_OP) { *out = 0; return 1; }
    if (*p == AML_ONE_OP)  { *out = 1; return 1; }
    if (*p == AML_BYTE_OP) {
        if (p + 1 >= end) return 0;
        *out = p[1];
        return 2;
    }
    if (*p == AML_WORD_OP) {
        if (p + 2 >= end) return 0;
        *out = (uint64_t)p[1] | ((uint64_t)p[2] << 8);
        return 3;
    }
    if (*p == AML_DWORD_OP) {
        if (p + 4 >= end) return 0;
        *out = (uint64_t)p[1] | ((uint64_t)p[2] << 8) |
               ((uint64_t)p[3] << 16) | ((uint64_t)p[4] << 24);
        return 5;
    }
    if (*p == AML_QWORD_OP) {
        if (p + 8 >= end) return 0;
        *out = 0;
        for (int i = 0; i < 8; i++)
            *out |= (uint64_t)p[1 + i] << (i * 8);
        return 9;
    }
    return 0;
}

/* PkgLength: 1-4 bytes. Top 2 bits of byte 0 = number of follow-up
 * bytes. Bits 0..3 of byte 0 are the low nibble; subsequent bytes
 * shift in 8 bits at a time. Returns total bytes consumed and length
 * value. */
static int decode_pkg_length(const uint8_t *p, const uint8_t *end,
                             uint32_t *out_len)
{
    if (p >= end) return 0;
    uint8_t b0 = *p;
    int extra = (b0 >> 6) & 0x3;
    if (p + extra >= end) return 0;
    uint32_t len;
    if (extra == 0) {
        len = b0 & 0x3F;
    } else {
        len = b0 & 0x0F;
        for (int i = 0; i < extra; i++) {
            len |= ((uint32_t)p[1 + i]) << (4 + 8 * i);
        }
    }
    *out_len = len;
    return 1 + extra;
}

/* ── Pattern: find Name (_BIF / _BST, Package(...)) ────────────── */
/*
 * AML for `Name (_BST, Package(N) {a, b, c, d})`:
 *   08 5F 42 53 54 12 <pkglen> <NumElements> <e0> <e1> ...
 * where elements are integer constants we decode with decode_int().
 *
 * We accept up to MAX_PKG_ELEMS elements. Anything we can't decode
 * cleanly leaves the corresponding field 0 / unknown.
 */
#define MAX_PKG_ELEMS 16

static int find_named_package(const uint8_t *aml, uint32_t len,
                              const char *name4,
                              uint64_t *elems, int *n_elems)
{
    /* Pattern: 08 <name4> 12 <pkglen> <NumElements> ... */
    for (uint32_t i = 0; i + 8 < len; i++) {
        if (aml[i] != AML_NAME_OP) continue;
        if (aml[i+1] != (uint8_t)name4[0]) continue;
        if (aml[i+2] != (uint8_t)name4[1]) continue;
        if (aml[i+3] != (uint8_t)name4[2]) continue;
        if (aml[i+4] != (uint8_t)name4[3]) continue;
        if (aml[i+5] != AML_PACKAGE_OP) continue;

        const uint8_t *end = aml + len;
        const uint8_t *p = aml + i + 6;
        uint32_t pkglen = 0;
        int c = decode_pkg_length(p, end, &pkglen);
        if (c <= 0) continue;
        p += c;
        if (p >= end) continue;
        uint8_t num = *p++;
        if (num > MAX_PKG_ELEMS) num = MAX_PKG_ELEMS;

        int got = 0;
        for (int k = 0; k < num && p < end; k++) {
            uint64_t v = 0;
            int cn = decode_int(p, end, &v);
            if (cn == 0) {
                /* Element is something we don't decode (string, ref,
                 * etc.). Stop — we still report what we got. */
                break;
            }
            elems[got++] = v;
            p += cn;
        }
        *n_elems = got;
        return got > 0 ? 0 : -1;
    }
    return -1;
}

/* ── HID search ────────────────────────────────────────────────── */
/*
 * Look for an AML byte sequence:
 *   08 5F 48 49 44   ("Name(_HID, ...)")
 *   0C <hid_dword>
 *
 * If matched, report the device exists. We don't bother locating the
 * enclosing Device() scope — for presence detection that's enough.
 */
static int hid_present(const uint8_t *aml, uint32_t len, const uint8_t hid[4])
{
    /* 08 5F 48 49 44  = Name(_HID,  */
    for (uint32_t i = 0; i + 10 < len; i++) {
        if (aml[i]   != AML_NAME_OP) continue;
        if (aml[i+1] != 0x5F) continue;
        if (aml[i+2] != 0x48) continue;  /* H */
        if (aml[i+3] != 0x49) continue;  /* I */
        if (aml[i+4] != 0x44) continue;  /* D */
        if (aml[i+5] != AML_DWORD_OP) continue;
        if (aml[i+6] != hid[0]) continue;
        if (aml[i+7] != hid[1]) continue;
        if (aml[i+8] != hid[2]) continue;
        if (aml[i+9] != hid[3]) continue;
        return 1;
    }
    return 0;
}

/* ── _BIF / _BST decoding ──────────────────────────────────────── */
/*
 * _BIF Package layout (ACPI 6.x, integer fields only, others are
 * strings we ignore):
 *   [0] Power Unit       (0=mWh, 1=mAh)
 *   [1] Design Capacity
 *   [2] Last Full Charge Capacity
 *   [3] Battery Technology
 *   [4] Design Voltage
 *   [5] Design Capacity of Warning
 *   [6] Design Capacity of Low
 *   ... strings follow
 *
 * _BST Package layout:
 *   [0] Battery State (bitmap: 1=discharging, 2=charging, 4=critical)
 *   [1] Present Rate (mW or mA)
 *   [2] Remaining Capacity (mWh or mAh)
 *   [3] Battery Voltage (mV)
 */
static void decode_bif(const uint8_t *aml, uint32_t len, battery_info_t *b)
{
    uint64_t elems[MAX_PKG_ELEMS];
    int n = 0;
    if (find_named_package(aml, len, "_BIF", elems, &n) != 0) return;
    if (n >= 1) b->power_unit      = (elems[0] == 1) ? 1 : 0;
    if (n >= 2) b->design_capacity = (uint32_t)elems[1];
    if (n >= 3) b->last_full       = (uint32_t)elems[2];
    if (b->design_capacity || b->last_full) b->values_known = 1;
}

static void decode_bst(const uint8_t *aml, uint32_t len, battery_info_t *b)
{
    uint64_t elems[MAX_PKG_ELEMS];
    int n = 0;
    if (find_named_package(aml, len, "_BST", elems, &n) != 0) return;
    if (n >= 1) {
        uint32_t s = (uint32_t)elems[0];
        if (s & 0x4) b->state = BAT_CRITICAL;
        else if (s & 0x2) b->state = BAT_CHARGING;
        else if (s & 0x1) b->state = BAT_DISCHARGING;
        else              b->state = BAT_FULL;
    }
    if (n >= 2) b->rate      = (uint32_t)elems[1];
    if (n >= 3) b->remaining = (uint32_t)elems[2];
    if (b->remaining || b->rate) b->values_known = 1;
}

/* ── _PSR (Power Source) ───────────────────────────────────────── */
/*
 * `Name (_PSR, Zero/One)` form OR a Method we cannot evaluate. We
 * scan for a Name(_PSR, <int>) literal — if absent, ac_online stays
 * unknown (-1).
 */
static int decode_psr_static(const uint8_t *aml, uint32_t len, int *out)
{
    /* 08 5F 50 53 52  = Name(_PSR,  */
    for (uint32_t i = 0; i + 6 < len; i++) {
        if (aml[i]   != AML_NAME_OP) continue;
        if (aml[i+1] != 0x5F) continue;
        if (aml[i+2] != 0x50) continue; /* P */
        if (aml[i+3] != 0x53) continue; /* S */
        if (aml[i+4] != 0x52) continue; /* R */
        uint64_t v = 0;
        int c = decode_int(aml + i + 5, aml + len, &v);
        if (c == 0) continue;
        *out = (v ? 1 : 0);
        return 0;
    }
    return -1;
}

/* ── AML evaluator fallback ────────────────────────────────────── *
 *
 * When pattern-scan can't pull static _BST/_BIF (because the firmware
 * implemented them as Methods), try invoking them through aml.c. We
 * try the conventional paths laptops use (\_SB_.PCI0.LPCB.EC0.BAT0,
 * \_SB_.PCI0.LPC0.EC.BAT0, \_SB_.BAT0, ...).
 */
static const char *kBatPaths[] = {
    "\\_SB_.PCI0.LPCB.EC0.BAT0._BST",
    "\\_SB_.PCI0.LPC0.EC.BAT0._BST",
    "\\_SB_.PCI0.LPC.EC0.BAT0._BST",
    "\\_SB_.BAT0._BST",
    "\\_SB_.BAT1._BST",
    0
};
static const char *kBifPaths[] = {
    "\\_SB_.PCI0.LPCB.EC0.BAT0._BIF",
    "\\_SB_.PCI0.LPC0.EC.BAT0._BIF",
    "\\_SB_.PCI0.LPC.EC0.BAT0._BIF",
    "\\_SB_.BAT0._BIF",
    "\\_SB_.BAT1._BIF",
    0
};

static int try_aml_bst(battery_info_t *b)
{
    aml_object_t r = { .type = AML_T_UNINIT };
    int rc = -1;
    for (int i = 0; kBatPaths[i]; i++) {
        rc = aml_evaluate(kBatPaths[i], 0, 0, &r);
        if (rc == 0 && r.type == AML_T_PACKAGE) break;
        aml_object_release(&r);
        r.type = AML_T_UNINIT;
    }
    if (rc != 0 || r.type != AML_T_PACKAGE) {
        aml_object_release(&r);
        return -1;
    }
    if (r.u.pkg.n >= 1) {
        uint32_t s = (uint32_t)(r.u.pkg.elems[0].type == AML_T_INTEGER ?
                                r.u.pkg.elems[0].u.integer : 0);
        if (s & 0x4) b->state = BAT_CRITICAL;
        else if (s & 0x2) b->state = BAT_CHARGING;
        else if (s & 0x1) b->state = BAT_DISCHARGING;
        else              b->state = BAT_FULL;
    }
    if (r.u.pkg.n >= 2 && r.u.pkg.elems[1].type == AML_T_INTEGER)
        b->rate = (uint32_t)r.u.pkg.elems[1].u.integer;
    if (r.u.pkg.n >= 3 && r.u.pkg.elems[2].type == AML_T_INTEGER)
        b->remaining = (uint32_t)r.u.pkg.elems[2].u.integer;
    if (b->remaining || b->rate) b->values_known = 1;
    aml_object_release(&r);
    return 0;
}

static int try_aml_bif(battery_info_t *b)
{
    aml_object_t r = { .type = AML_T_UNINIT };
    int rc = -1;
    for (int i = 0; kBifPaths[i]; i++) {
        rc = aml_evaluate(kBifPaths[i], 0, 0, &r);
        if (rc == 0 && r.type == AML_T_PACKAGE) break;
        aml_object_release(&r);
        r.type = AML_T_UNINIT;
    }
    if (rc != 0 || r.type != AML_T_PACKAGE) {
        aml_object_release(&r);
        return -1;
    }
    if (r.u.pkg.n >= 1 && r.u.pkg.elems[0].type == AML_T_INTEGER)
        b->power_unit = (r.u.pkg.elems[0].u.integer == 1) ? 1 : 0;
    if (r.u.pkg.n >= 2 && r.u.pkg.elems[1].type == AML_T_INTEGER)
        b->design_capacity = (uint32_t)r.u.pkg.elems[1].u.integer;
    if (r.u.pkg.n >= 3 && r.u.pkg.elems[2].type == AML_T_INTEGER)
        b->last_full = (uint32_t)r.u.pkg.elems[2].u.integer;
    if (b->design_capacity || b->last_full) b->values_known = 1;
    aml_object_release(&r);
    return 0;
}

/* ── Public API ────────────────────────────────────────────────── */

void battery_init(void)
{
    if (g_initialized) return;
    g_initialized = 1;

    for (int i = 0; i < BATTERY_MAX; i++) {
        g_bats[i].present       = 0;
        g_bats[i].values_known  = 0;
        g_bats[i].design_capacity = 0;
        g_bats[i].last_full     = 0;
        g_bats[i].remaining     = 0;
        g_bats[i].rate          = 0;
        g_bats[i].power_unit    = -1;
        g_bats[i].state         = BAT_UNKNOWN;
    }
    g_bat_count = 0;
    g_ac_present_known = 0;
    g_ac_online = 0;

    int n_tables = acpi_aml_table_count();
    if (n_tables == 0) {
        kputs("[battery] no AML tables (ACPI not initialized)\n");
        return;
    }

    /* Walk every AML table looking for battery and AC HIDs. We treat
     * each match-for-battery-HID as a distinct battery device up to
     * BATTERY_MAX. Most laptops have exactly 1; a few expose 2. */
    int found_bat = 0;
    int psr_seen = -1;
    for (int t = 0; t < n_tables; t++) {
        const acpi_aml_table_t *tab = acpi_aml_table(t);
        if (!tab || !tab->aml) continue;

        if (hid_present(tab->aml, tab->len, HID_BATTERY) && found_bat < BATTERY_MAX) {
            battery_info_t *b = &g_bats[found_bat++];
            b->present = 1;
            decode_bif(tab->aml, tab->len, b);
            decode_bst(tab->aml, tab->len, b);
            /* Method-form fallback via AML interpreter. Only fills in
             * fields that pattern-scan didn't already populate. */
            if (!b->values_known) {
                (void)try_aml_bif(b);
                (void)try_aml_bst(b);
            }
        }
        if (hid_present(tab->aml, tab->len, HID_AC)) {
            g_ac_present_known = 1;
            int v = -1;
            if (decode_psr_static(tab->aml, tab->len, &v) == 0) {
                g_ac_online = v;
                psr_seen = v;
            }
        }
    }
    g_bat_count = found_bat;

    kputs("[battery] init: ");
    kput_dec((uint64_t)g_bat_count);
    kputs(" battery device(s), AC=");
    if (g_ac_present_known) {
        if (psr_seen == 1)      kputs("online");
        else if (psr_seen == 0) kputs("offline");
        else                    kputs("present (unknown state)");
    } else {
        kputs("not detected");
    }
    kputs("\n");
}

int battery_present(void)
{
    return g_bat_count;
}

int battery_count(void) { return g_bat_count; }

int battery_percent(int idx)
{
    if (idx < 0 || idx >= g_bat_count) return -1;
    const battery_info_t *b = &g_bats[idx];
    if (!b->values_known)   return -1;
    if (!b->last_full)      return -1;
    if (b->remaining > b->last_full) return 100;
    uint64_t pct = ((uint64_t)b->remaining * 100ULL) / (uint64_t)b->last_full;
    if (pct > 100) pct = 100;
    return (int)pct;
}

int battery_remaining_minutes(int idx)
{
    if (idx < 0 || idx >= g_bat_count) return -1;
    const battery_info_t *b = &g_bats[idx];
    if (!b->values_known) return -1;
    if (!b->rate)         return -1; /* not currently draining/charging at known rate */
    if (b->state == BAT_DISCHARGING) {
        if (!b->remaining) return -1;
        return (int)(((uint64_t)b->remaining * 60ULL) / (uint64_t)b->rate);
    }
    if (b->state == BAT_CHARGING) {
        if (b->remaining >= b->last_full) return 0;
        uint32_t deficit = b->last_full - b->remaining;
        return (int)(((uint64_t)deficit * 60ULL) / (uint64_t)b->rate);
    }
    return -1;
}

battery_state_t battery_state(int idx)
{
    if (idx < 0 || idx >= g_bat_count) return BAT_UNKNOWN;
    return g_bats[idx].state;
}

int ac_online(void)
{
    if (!g_ac_present_known) return -1;
    return g_ac_online;
}

int battery_refresh(void)
{
    /* Without a real AML interpreter we can only re-read whatever is
     * inlined in the AML byte stream — which doesn't change between
     * calls. We rescan anyway so that the moment an interpreter
     * lands, polling Just Works. Returns 1 if anything actually
     * differs from the cache. */
    int n_tables = acpi_aml_table_count();
    if (n_tables == 0) return 0;

    battery_info_t old[BATTERY_MAX];
    int old_ac_online = g_ac_online;
    int old_ac_known  = g_ac_present_known;
    for (int i = 0; i < BATTERY_MAX; i++) old[i] = g_bats[i];

    /* Reset only the volatile fields; presence flags persist. */
    for (int i = 0; i < g_bat_count; i++) {
        g_bats[i].state         = BAT_UNKNOWN;
        g_bats[i].rate          = 0;
        g_bats[i].remaining     = 0;
        g_bats[i].values_known  = 0;
    }

    int b_idx = 0;
    for (int t = 0; t < n_tables && b_idx < g_bat_count; t++) {
        const acpi_aml_table_t *tab = acpi_aml_table(t);
        if (!tab || !tab->aml) continue;
        if (hid_present(tab->aml, tab->len, HID_BATTERY)) {
            decode_bif(tab->aml, tab->len, &g_bats[b_idx]);
            decode_bst(tab->aml, tab->len, &g_bats[b_idx]);
            if (!g_bats[b_idx].values_known) {
                (void)try_aml_bif(&g_bats[b_idx]);
                (void)try_aml_bst(&g_bats[b_idx]);
            }
            b_idx++;
        }
        if (hid_present(tab->aml, tab->len, HID_AC)) {
            int v = -1;
            if (decode_psr_static(tab->aml, tab->len, &v) == 0) {
                g_ac_online = v;
            }
        }
    }

    int changed = 0;
    if (old_ac_online != g_ac_online) changed = 1;
    if (old_ac_known != g_ac_present_known) changed = 1;
    for (int i = 0; i < g_bat_count; i++) {
        if (old[i].state != g_bats[i].state) changed = 1;
        if (old[i].remaining != g_bats[i].remaining) changed = 1;
        /* percent crossings */
        int op = old[i].last_full ? (int)(((uint64_t)old[i].remaining*100)/old[i].last_full) : -1;
        int np = battery_percent(i);
        const int thresh[] = {5, 10, 20, 50, 80, 100};
        for (unsigned k = 0; k < sizeof(thresh)/sizeof(thresh[0]); k++) {
            int cross = ((op < thresh[k]) != (np < thresh[k]));
            if (cross && op != np) { changed = 1; break; }
        }
    }
    return changed;
}

const battery_info_t *battery_info(int idx)
{
    if (idx < 0 || idx >= g_bat_count) return 0;
    return &g_bats[idx];
}
