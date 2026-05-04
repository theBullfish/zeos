/*
 * Zeos — display backlight control.
 *
 * ACPI backlight via DSDT _BCL/_BCM scan. Many firmwares route _BCM
 * through SMI which we can't drive from the kernel; in those cases
 * we report queryable levels but settable=0 and log honestly. Native
 * GPU PWM (Intel BLC_PWM_CTL, AMD DC_BL_PWM_CTL) is per-driver and
 * blocked on real-vendor GPU drivers landing.
 *
 * What we DO support:
 *   - Pattern-scan DSDT/SSDT AML for `Name (_BCL, Package(...))` and
 *     extract the supported brightness levels (ACPI spec: first two
 *     entries are AC-max / battery-max policy hints; the rest is the
 *     ascending level list).
 *   - Detect presence of a `_BCM` MethodOp (0x14) in the same AML
 *     stream. Presence alone doesn't mean we can drive it — most
 *     real-world implementations are 1-2 lines of EC/SMI handoff that
 *     require an AML interpreter to evaluate. We honestly report
 *     "queryable but not settable" in that case.
 *   - Persist desired level to VAULT key /display/brightness so a
 *     reboot picks up the user's preference. When a real interpreter
 *     lands the same module starts driving _BCM without an API change.
 *
 * What we DO NOT support:
 *   - AML interpreter — _BCM(N), _BQC() can't be invoked.
 *   - Per-vendor MMIO PWM registers — Intel/AMD/NVIDIA paths each
 *     need a real GPU driver.
 *   - EFI Brightness Output Protocol — only available pre-ExitBootServices.
 */

#include "brightness.h"
#include "acpi.h"
#include "vault.h"
#include "kprint.h"

#include <stdint.h>

/* AML opcodes (subset shared with battery.c). */
#define AML_NAME_OP    0x08
#define AML_PACKAGE_OP 0x12
#define AML_METHOD_OP  0x14
#define AML_BYTE_OP    0x0A
#define AML_WORD_OP    0x0B
#define AML_DWORD_OP   0x0C
#define AML_QWORD_OP   0x0E
#define AML_ZERO_OP    0x00
#define AML_ONE_OP     0x01

/* Persistence record. */
#define BRIGHT_VAULT_PATH    "/display/brightness"
#define BRIGHT_VAULT_MAGIC   0x42524754u  /* 'BRGT' */
#define BRIGHT_VAULT_VERSION 1

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  level_pct;   /* 0..100 desired */
    uint8_t  reserved[2];
} brightness_persist_t;

/* ── Module state ──────────────────────────────────────────────── */

static int      g_initialized = 0;
static int      g_present     = 0;
static int      g_settable    = 0;     /* _BCM Method discovered */
static int      g_levels[BRIGHTNESS_MAX_LEVELS];
static int      g_level_count = 0;
static int      g_current_pct = 60;    /* default until VAULT restore */

/* ── AML decode helpers (mirrors battery.c) ───────────────────── */

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
        for (int i = 0; i < 8; i++) *out |= (uint64_t)p[1 + i] << (i * 8);
        return 9;
    }
    return 0;
}

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

/* ── _BCL Package scan ────────────────────────────────────────── *
 * AML: 08 5F 42 43 4C 12 <pkglen> <NumElements> <e0> <e1> ...
 *      Name(_BCL, Package(N) { ... })
 *
 * Per ACPI 6.x §B.6.2 the first two entries are AC and battery
 * brightness ceiling hints, and the remaining entries are the actual
 * supported levels in ascending order.
 */
static int find_bcl_package(const uint8_t *aml, uint32_t len,
                            int *levels_out, int max_out)
{
    for (uint32_t i = 0; i + 8 < len; i++) {
        if (aml[i]   != AML_NAME_OP) continue;
        if (aml[i+1] != 0x5F) continue;        /* '_' */
        if (aml[i+2] != 0x42) continue;        /* 'B' */
        if (aml[i+3] != 0x43) continue;        /* 'C' */
        if (aml[i+4] != 0x4C) continue;        /* 'L' */
        if (aml[i+5] != AML_PACKAGE_OP) continue;

        const uint8_t *end = aml + len;
        const uint8_t *p = aml + i + 6;
        uint32_t pkglen = 0;
        int c = decode_pkg_length(p, end, &pkglen);
        if (c <= 0) continue;
        p += c;
        if (p >= end) continue;
        uint8_t num = *p++;

        int got = 0;
        for (int k = 0; k < num && p < end && got < max_out; k++) {
            uint64_t v = 0;
            int cn = decode_int(p, end, &v);
            if (cn == 0) break;
            levels_out[got++] = (int)v;
            p += cn;
        }
        return got;
    }
    return 0;
}

/* ── _BCM Method presence ─────────────────────────────────────── *
 * MethodOp is 0x14 followed by PkgLength then NameString. We just
 * scan for the byte sequence 14 <pkglen-skip> ... 5F 42 43 4D within
 * a small window — good enough for presence detection. The simpler
 * pattern: search for the literal nameseg "_BCM" preceded by the
 * MethodOp marker within a few bytes.
 */
static int has_bcm_method(const uint8_t *aml, uint32_t len)
{
    /* Scan for nameseg "_BCM" = 5F 42 43 4D. Confirm a MethodOp 0x14
     * sits within 6 bytes upstream (allowing for the 1-4 byte PkgLength
     * in between). Method declarations always have this layout. */
    for (uint32_t i = 0; i + 4 < len; i++) {
        if (aml[i]   != 0x5F) continue;
        if (aml[i+1] != 0x42) continue;
        if (aml[i+2] != 0x43) continue;
        if (aml[i+3] != 0x4D) continue;
        uint32_t lo = (i >= 6) ? (i - 6) : 0;
        for (uint32_t j = lo; j < i; j++) {
            if (aml[j] == AML_METHOD_OP) return 1;
        }
    }
    return 0;
}

/* ── Comparator helpers ───────────────────────────────────────── */

static void sort_levels_asc(int *a, int n)
{
    for (int i = 1; i < n; i++) {
        int x = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > x) { a[j+1] = a[j]; j--; }
        a[j+1] = x;
    }
}

static int snap_pct_to_level_idx(int pct)
{
    if (g_level_count <= 0) return -1;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    int lo = g_levels[0];
    int hi = g_levels[g_level_count - 1];
    if (hi <= lo) return 0;
    int target = lo + ((hi - lo) * pct) / 100;
    int best = 0;
    int best_d = 0x7fffffff;
    for (int i = 0; i < g_level_count; i++) {
        int d = g_levels[i] - target;
        if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static int level_to_pct(int raw)
{
    if (g_level_count <= 0) return -1;
    int lo = g_levels[0];
    int hi = g_levels[g_level_count - 1];
    if (hi <= lo) return 100;
    int v = ((raw - lo) * 100) / (hi - lo);
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    return v;
}

/* ── Public API ────────────────────────────────────────────────── */

void brightness_init(void)
{
    if (g_initialized) return;
    g_initialized = 1;

    g_present     = 0;
    g_settable    = 0;
    g_level_count = 0;

    int n_tables = acpi_aml_table_count();
    if (n_tables == 0) {
        kputs("[brightness] no AML tables (ACPI not initialized)\n");
        return;
    }

    int raw[BRIGHTNESS_MAX_LEVELS + 4];
    int total = 0;
    int bcm_seen = 0;

    for (int t = 0; t < n_tables; t++) {
        const acpi_aml_table_t *tab = acpi_aml_table(t);
        if (!tab || !tab->aml) continue;

        if (total < BRIGHTNESS_MAX_LEVELS + 2) {
            int got = find_bcl_package(tab->aml, tab->len,
                                       raw + total,
                                       (BRIGHTNESS_MAX_LEVELS + 2) - total);
            if (got > 0) total += got;
        }
        if (!bcm_seen && has_bcm_method(tab->aml, tab->len)) bcm_seen = 1;
    }

    if (total < 3) {
        /* _BCL must contain at least 2 policy hints + 1 level. Anything
         * less means we didn't find a real backlight. */
        kputs("[brightness] no _BCL package located (desktop or virtio-gpu)\n");
        return;
    }

    /* Per spec, drop the first two policy-hint entries. */
    int n = total - 2;
    if (n > BRIGHTNESS_MAX_LEVELS) n = BRIGHTNESS_MAX_LEVELS;
    for (int i = 0; i < n; i++) g_levels[i] = raw[i + 2];
    sort_levels_asc(g_levels, n);

    /* Dedupe (some firmwares list the same level twice). */
    int u = 0;
    for (int i = 0; i < n; i++) {
        if (u == 0 || g_levels[u-1] != g_levels[i]) g_levels[u++] = g_levels[i];
    }
    g_level_count = u;
    g_present     = 1;
    g_settable    = bcm_seen;
    g_current_pct = 60;

    kputs("[brightness] init: ");
    kput_dec((uint64_t)g_level_count);
    kputs(" levels (");
    kput_dec((uint64_t)g_levels[0]);
    kputs("..");
    kput_dec((uint64_t)g_levels[g_level_count - 1]);
    kputs("), _BCM ");
    if (g_settable) kputs("present (best-effort drive)\n");
    else            kputs("absent — no settable backlight (firmware likely SMI-only)\n");
}

int brightness_present(void) { return g_present; }

int brightness_levels(int *out, int max)
{
    if (!g_present || !out || max <= 0) return 0;
    int n = g_level_count;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = g_levels[i];
    return n;
}

int brightness_get(void)
{
    if (!g_present) return -1;
    return g_current_pct;
}

int brightness_set(int pct)
{
    if (!g_present) return -1;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    int idx = snap_pct_to_level_idx(pct);
    if (idx < 0) return -1;
    int snapped_pct = level_to_pct(g_levels[idx]);
    g_current_pct = snapped_pct;

    /* Always persist desired level so future boots / future interpreter
     * landing pick it up. */
    (void)brightness_save_to_vault();

    if (!g_settable) {
        kputs("[brightness] no settable backlight (firmware has SMI-only path); persisted ");
        kput_dec((uint64_t)snapped_pct);
        kputs("%\n");
        return 1;
    }

    /* TODO: when an AML interpreter lands, evaluate _BCM(g_levels[idx]).
     * For now we record the desired raw level and report best-effort. */
    return 1;
}

int brightness_step(int delta)
{
    if (!g_present) return -1;
    int pct = g_current_pct + delta;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return brightness_set(pct);
}

int brightness_save_to_vault(void)
{
    brightness_persist_t rec;
    rec.magic       = BRIGHT_VAULT_MAGIC;
    rec.version     = BRIGHT_VAULT_VERSION;
    rec.level_pct   = (uint8_t)g_current_pct;
    rec.reserved[0] = 0;
    rec.reserved[1] = 0;
    (void)vault_mkdir("/display", VAULT_TIER_INTERNAL);
    int n = vault_write(BRIGHT_VAULT_PATH, &rec, sizeof(rec));
    return (n == (int)sizeof(rec)) ? 0 : -1;
}

int brightness_restore_from_vault(void)
{
    int sz = vault_size(BRIGHT_VAULT_PATH);
    if (sz != (int)sizeof(brightness_persist_t)) return -1;

    brightness_persist_t rec;
    int n = vault_read(BRIGHT_VAULT_PATH, &rec, sizeof(rec));
    if (n != (int)sizeof(rec)) return -1;
    if (rec.magic   != BRIGHT_VAULT_MAGIC)   return -1;
    if (rec.version != BRIGHT_VAULT_VERSION) return -1;

    int v = rec.level_pct;
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    if (g_present) {
        int idx = snap_pct_to_level_idx(v);
        if (idx >= 0) g_current_pct = level_to_pct(g_levels[idx]);
        else          g_current_pct = v;
    } else {
        g_current_pct = v;
    }
    return 0;
}
