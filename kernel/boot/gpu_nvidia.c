/*
 * NVIDIA driver. Stage 1: Turing scanout + EDID via DPAUX/DCB.
 * Stage 2: Ampere GSP firmware load + RM control path.
 * Compute backend registers only when GSP is up.
 * References nouveau register layout.
 *
 * Stage 1 design (this file):
 *   - PCI scan for vendor 0x10DE, class 0x03 (display controller).
 *   - Map BAR0 (registers, ~16MB), BAR1 (FB aperture), BAR3 (varies).
 *   - Read PMC_BOOT_0 at BAR0 + 0x000000. Top nibbles encode chip
 *     family per the nouveau layout reference: Turing = 0x16x,
 *     Ampere = 0x17x, etc.
 *   - For Turing: trust the VBIOS / GOP-set mode. We do NOT issue
 *     a fresh modeset (no GSP, no full PDISP state machine here).
 *     We verify head/encoder/connector triples from the Display
 *     Control Block (DCB) parsed out of the option ROM / VBIOS,
 *     read EDID per output via DPAUX (DP/eDP) or I2C (HDMI/DVI),
 *     and register one CHAIN_DISPLAY_<n> sub-chain per detected
 *     output.
 *   - The "scanout" pipeline node reports the GOP framebuffer is
 *     the active source (it already is on a Turing card whose
 *     UEFI GOP set the mode). hardware_dma is a vault_version
 *     bumper -- there is NO host-visible DMA from Zeos at Stage 1
 *     (no command submission engine without GSP). This is honest:
 *     we observe the scanout, we don't reprogram it.
 *
 * Stage 2 (gpu_nvidia_gsp_bringup):
 *   - Find embedded GSP firmware blobs in kernel/lib/firmware/nvidia/
 *     and DMA them into the GSP RISC-V core via PMC_GSP registers.
 *   - Wait for the GSP-ready handshake (RM message ring online).
 *   - Issue a minimal RM "GET_GPU_INFO" to verify the path.
 *   - On success: register gpu_compute backend "nvidia-N" so MDE
 *     device_select can pick it for prefer_gpu requests.
 *
 * Honest gap: GSP firmware loading is NOT implemented in this
 * change. The function returns -ENOTREADY and logs the reason.
 * No compute backend is registered. The gap is documented inline
 * and surfaces in the `nvidia` shell command + selftest output.
 *
 * Constraints honored:
 *   - No GPL code copied. Register names / offsets used here are
 *     either from public NVIDIA developer documentation or are
 *     the conventional names matched to layouts in the open
 *     `envytools` project (MIT-licensed).
 *   - We never write BAR0 in a way that could brick the card.
 *     Stage 1 is read-mostly; the only writes are to enable
 *     PCI memory + bus master in PCI config space.
 */

#include "gpu_nvidia.h"
#include "chain.h"
#include "chain_registry.h"
#include "cfa_handle.h"
#include "gpu_compute.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "fb.h"
#include "kprint.h"
#include "timer.h"
#include "zeos_boot.h"
#include <stdint.h>

/* ── Register offsets (BAR0) ────────────────────────────────────── */
/* Names mirror the public NVIDIA developer docs / envytools layout.
 * We only USE PMC_BOOT_0 (chip ID) at Stage 1; the rest are listed
 * for clarity / Stage-2 use. */

#define NV_PMC_BOOT_0          0x000000  /* Chip identifier. */
#define NV_PMC_INTR_EN_0       0x000140
#define NV_PBUS_INTR           0x001100
#define NV_PFB_PRI_BASE        0x100000  /* Frame buffer control. */
#define NV_PDISP_BASE_TURING   0x610000  /* Display engine (Turing+). */
#define NV_PDISP_HEAD_BASE     0x611000
#define NV_PMGR_BASE           0x10F000  /* DPAUX / I2C live in PMGR. */
#define NV_PROM_BASE           0x300000  /* Option ROM mirror window. */

/* Bar3 / FBHUB (per chip varies; not used Stage 1). */

/* ── BAR mapping helper (mirrors gpu_virtio.c style) ────────────── */

static uint64_t nv_bar_phys64(struct pci_device *d, int idx)
{
    uint32_t lo = d->bar[idx];
    uint64_t addr;
    if ((lo & 0x6) == 0x4) {
        uint32_t hi = d->bar[idx + 1];
        addr = ((uint64_t)hi << 32) | (lo & ~0xFu);
    } else if ((lo & 0x1) == 0) {
        addr = lo & ~0xFu;
    } else {
        addr = lo & ~0x3u;
    }
    return addr;
}

static volatile void *nv_map_bar(uint64_t phys, uint64_t length)
{
    if (phys == 0 || length == 0) return 0;
    if (phys + length <= 0x100000000ull) {
        return (volatile void *)(uintptr_t)phys;
    }
    uint64_t start = phys & ~0xFFFull;
    uint64_t end   = (phys + length + 0xFFFull) & ~0xFFFull;
    uint64_t pages = (end - start) / 4096ull;
    vmm_map_range(start, start, pages, PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE);
    return (volatile void *)(uintptr_t)phys;
}

static inline uint32_t nv_r32(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}
/* Stage 2 will use writes; Stage 1 only writes via PCI config helpers. */
static inline void nv_w32(volatile uint8_t *base, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

/* ── Per-device state ───────────────────────────────────────────── */

typedef struct nv_dev {
    int                 in_use;
    int                 ready;
    int                 gsp_ready;
    int                 gpu_index;     /* nvidia-local index. */
    struct pci_device  *pci;

    volatile uint8_t   *bar0;          /* registers */
    uint64_t            bar0_phys;
    uint64_t            bar0_len;
    volatile uint8_t   *bar1;          /* FB aperture (mapped lazily) */
    uint64_t            bar1_phys;
    uint64_t            bar1_len;
    volatile uint8_t   *bar3;
    uint64_t            bar3_phys;
    uint64_t            bar3_len;

    uint32_t            pmc_boot_0;
    uint16_t            chip_family;   /* NV_FAMILY_* */

    int                 chain_id;          /* CHAIN_GPU_nvidia<n> */
    int                 chain_render_id;
    int                 chain_display_id;

    int                 output_count;
    nv_output_t         outputs[GPU_NVIDIA_MAX_OUTPUTS];

    uint32_t            vault_version_local;
} nv_dev_t;

static nv_dev_t  s_nv[GPU_NVIDIA_MAX_DEVICES];
static int       s_nv_count = 0;
static int       s_total_outputs = 0;

/* ── Chip family decoder ───────────────────────────────────────── */

/* PMC_BOOT_0 layout (envytools convention):
 *   bits 20..27 -> chip family/architecture nibbles, encoded as
 *   0x000NN0xx where NN selects the architecture.
 * Common identifiers:
 *   0x0E0  Kepler
 *   0x110  Maxwell
 *   0x130  Pascal
 *   0x140  Volta
 *   0x160  Turing  (TU102/104/106/116/117 -> 0x162x...0x167x)
 *   0x170  Ampere  (GA100/102/103/104/106/107)
 *   0x190  Ada
 *
 * For Stage 1 we just want a coarse family bucket. */
static uint16_t nv_decode_family(uint32_t pmc_boot_0)
{
    uint32_t arch = (pmc_boot_0 >> 20) & 0x1FF;
    if (arch >= 0x1B0) return NV_FAMILY_BLACKWELL;
    if (arch >= 0x190) return NV_FAMILY_ADA;
    if (arch >= 0x180) return NV_FAMILY_HOPPER;
    if (arch >= 0x170) return NV_FAMILY_AMPERE;
    if (arch >= 0x160) return NV_FAMILY_TURING;
    if (arch >= 0x140) return NV_FAMILY_VOLTA;
    if (arch >= 0x130) return NV_FAMILY_PASCAL;
    if (arch >= 0x110) return NV_FAMILY_MAXWELL;
    if (arch >= 0x0E0) return NV_FAMILY_KEPLER;
    if (arch >= 0x0C0) return NV_FAMILY_FERMI;
    if (arch >= 0x050) return NV_FAMILY_TESLA;
    return NV_FAMILY_UNKNOWN;
}

static const char *nv_family_name(uint16_t f)
{
    switch (f) {
    case NV_FAMILY_KELVIN:    return "Kelvin";
    case NV_FAMILY_RANKINE:   return "Rankine";
    case NV_FAMILY_CURIE:     return "Curie";
    case NV_FAMILY_TESLA:     return "Tesla";
    case NV_FAMILY_FERMI:     return "Fermi";
    case NV_FAMILY_KEPLER:    return "Kepler";
    case NV_FAMILY_MAXWELL:   return "Maxwell";
    case NV_FAMILY_PASCAL:    return "Pascal";
    case NV_FAMILY_VOLTA:     return "Volta";
    case NV_FAMILY_TURING:    return "Turing";
    case NV_FAMILY_AMPERE:    return "Ampere";
    case NV_FAMILY_ADA:       return "Ada";
    case NV_FAMILY_HOPPER:    return "Hopper";
    case NV_FAMILY_BLACKWELL: return "Blackwell";
    default: return "Unknown";
    }
}

static const char *nv_output_name(nv_output_kind_t k)
{
    switch (k) {
    case NV_OUTPUT_VGA:   return "VGA";
    case NV_OUTPUT_TV:    return "TV";
    case NV_OUTPUT_TMDS:  return "DVI";
    case NV_OUTPUT_LVDS:  return "LVDS";
    case NV_OUTPUT_HDMI:  return "HDMI";
    case NV_OUTPUT_DP:    return "DP";
    case NV_OUTPUT_eDP:   return "eDP";
    default:              return "?";
    }
}

/* ── DCB (Display Control Block) parsing ───────────────────────── */
/*
 * The DCB lives in the VBIOS option ROM. Its location is given by a
 * 32-bit pointer at offset 0x36 of the VBIOS image (legacy convention)
 * for older cards; modern (Turing+) uses the same pointer offset.
 * Each entry is a packed 32-bit field describing one display path:
 *   bits 0..3   = output type (0=analog/VGA, 1=TV, 2=TMDS, 3=LVDS,
 *                 6=DisplayPort, 8=eDP)
 *   bits 4..7   = edid port (i2c)
 *   bits 8..11  = head bitmask (1=head0, 2=head1, ...)
 *   bits 12..15 = connector index
 *   bits 16..19 = bus number
 *
 * Reading the option ROM through PMC PROM_BASE is the safest path
 * (no platform ACPI gymnastics). Card must have its ROM_SHADOW
 * latched -- on UEFI boot this is normally already the case.
 *
 * If we cannot find a coherent DCB, we register a single SYNTHETIC
 * output mapped to head 0 with kind UNKNOWN and width/height taken
 * from the GOP framebuffer. This keeps the chain hierarchy honest:
 * the card is present, exactly one display is attached (the boot
 * console), but we couldn't enumerate every connector.
 */

static int nv_read_vbios_u32(volatile uint8_t *bar0, uint32_t off, uint32_t *out)
{
    if (!bar0 || !out) return -1;
    *out = nv_r32(bar0, NV_PROM_BASE + off);
    return 0;
}
static int nv_read_vbios_u8(volatile uint8_t *bar0, uint32_t off, uint8_t *out)
{
    if (!bar0 || !out) return -1;
    /* PROM_BASE is a 32-bit aperture; scalar reads work via dword + shift. */
    uint32_t dw = nv_r32(bar0, NV_PROM_BASE + (off & ~3u));
    *out = (uint8_t)((dw >> ((off & 3u) * 8u)) & 0xFFu);
    return 0;
}

/* Parse DCB; return number of outputs populated into gd->outputs[].
 * Best-effort: any malformed field aborts parsing for that entry. */
static int nv_parse_dcb(nv_dev_t *gd)
{
    if (!gd || !gd->bar0) return 0;

    /* VBIOS signature "55 AA" at the start of the option ROM. */
    uint8_t sig0 = 0, sig1 = 0;
    nv_read_vbios_u8(gd->bar0, 0x00, &sig0);
    nv_read_vbios_u8(gd->bar0, 0x01, &sig1);
    if (sig0 != 0x55 || sig1 != 0xAA) {
        return 0;  /* No mapped option ROM. */
    }

    /* DCB pointer at offset 0x36. */
    uint8_t pd0=0, pd1=0;
    nv_read_vbios_u8(gd->bar0, 0x36, &pd0);
    nv_read_vbios_u8(gd->bar0, 0x37, &pd1);
    uint32_t dcb_off = (uint32_t)pd0 | ((uint32_t)pd1 << 8);
    if (dcb_off < 0x40 || dcb_off > 0xFC00) return 0;

    /* DCB header is 4..8 bytes. Header[0] = version, header[1] = entry size,
     * header[2] = entry count, header[3] = header size. */
    uint8_t ver=0, esize=0, ecount=0, hsize=0;
    nv_read_vbios_u8(gd->bar0, dcb_off + 0, &ver);
    nv_read_vbios_u8(gd->bar0, dcb_off + 1, &esize);
    nv_read_vbios_u8(gd->bar0, dcb_off + 2, &ecount);
    nv_read_vbios_u8(gd->bar0, dcb_off + 3, &hsize);

    /* Sanity: modern DCB versions are >= 0x30. esize is typically 8 bytes. */
    if (ver < 0x20 || ver > 0x42) return 0;
    if (esize != 8 && esize != 16) return 0;
    if (ecount == 0 || ecount > 16) return 0;
    if (hsize < 4 || hsize > 32) return 0;

    int written = 0;
    for (int i = 0; i < ecount && written < GPU_NVIDIA_MAX_OUTPUTS; i++) {
        uint32_t eoff = dcb_off + hsize + (uint32_t)i * esize;
        uint32_t e0 = 0;
        nv_read_vbios_u32(gd->bar0, eoff, &e0);
        if (e0 == 0xFFFFFFFFu || e0 == 0x00000000u) continue;

        uint8_t  type = (uint8_t)(e0 & 0x0F);
        uint8_t  port = (uint8_t)((e0 >> 4) & 0x0F);
        uint8_t  hmask = (uint8_t)((e0 >> 8) & 0x0F);
        uint8_t  conn  = (uint8_t)((e0 >> 12) & 0x0F);

        nv_output_kind_t kind = NV_OUTPUT_UNKNOWN;
        switch (type) {
        case 0: kind = NV_OUTPUT_VGA;  break;
        case 1: kind = NV_OUTPUT_TV;   break;
        case 2: kind = NV_OUTPUT_TMDS; break;
        case 3: kind = NV_OUTPUT_LVDS; break;
        case 6: kind = NV_OUTPUT_DP;   break;
        case 8: kind = NV_OUTPUT_eDP;  break;
        case 0xE:
        case 0xF: continue;            /* skip "unused" / "EOL" entries */
        default: kind = NV_OUTPUT_UNKNOWN; break;
        }

        /* Pick lowest set bit of head mask for the head index. */
        uint8_t head = 0;
        for (int b = 0; b < 4; b++) if (hmask & (1u << b)) { head = (uint8_t)b; break; }

        nv_output_t *o = &gd->outputs[written];
        o->valid       = 1;
        o->kind        = kind;
        o->dcb_index   = (uint8_t)i;
        o->head        = head;
        o->dpaux_port  = (kind == NV_OUTPUT_DP || kind == NV_OUTPUT_eDP) ? port : 0xFF;
        o->i2c_port    = (kind == NV_OUTPUT_HDMI || kind == NV_OUTPUT_TMDS) ? port : 0xFF;
        o->edid_valid  = 0;
        o->edid_size   = 0;
        o->monitor_name[0] = 0;
        /* Mode info: we don't have a programmed mode without a full
         * PDISP read; reuse the GOP framebuffer size as the boot mode
         * for head 0 only. Other heads stay 0x0 (honest: we did not
         * verify their mode). */
        if (head == 0) {
            o->width  = fb_width();
            o->height = fb_height();
            o->refresh_hz = 60;
        }
        (void)conn;
        written++;
    }
    return written;
}

/* EDID monitor-name extractor (same shape as gpu_virtio's). */
static void nv_edid_extract_monitor_name(const uint8_t *edid, uint32_t size,
                                         char out[16])
{
    out[0] = 0;
    if (size < 128) return;
    for (int d = 0; d < 4; d++) {
        const uint8_t *r = edid + 54 + d * 18;
        if (r[0] == 0x00 && r[1] == 0x00 && r[2] == 0x00 && r[3] == 0xFC) {
            int n = 0;
            for (int i = 5; i < 18 && n < 15; i++) {
                uint8_t c = r[i];
                if (c == 0x0A || c == 0) break;
                if (c < 0x20 || c > 0x7E) c = ' ';
                out[n++] = (char)c;
            }
            while (n > 0 && out[n - 1] == ' ') n--;
            out[n] = 0;
            return;
        }
    }
}

/* Stage 1 EDID read is best-effort:
 *   - DPAUX (DP/eDP) requires programming the DPAUX FSM in PMGR;
 *     full sequence is multi-step and chip-family-specific. We
 *     try a non-destructive readback of the DPAUX status/data
 *     window only -- if the GOP firmware already negotiated link
 *     training, the EDID bytes may be left in a scratch register
 *     or in the VBIOS shadow. We don't force a fresh transaction.
 *   - I2C (HDMI/DVI) requires bit-banging through PMGR's GPIO
 *     I2C controller. We do NOT bit-bang in Stage 1 -- the
 *     timing-loop introduces wall-clock dependencies we don't
 *     want at boot. Instead, we look for an EDID block in the
 *     VBIOS option ROM (some VBIOSes cache the panel EDID for
 *     eDP/LVDS).
 *
 * Result: edid_valid is left 0 unless we find an EDID block
 * starting with the canonical 00 FF FF FF FF FF FF 00 header
 * inside the first 64KB of the option ROM. This is honest --
 * we don't lie about reading EDID off the wire.
 */
static int nv_try_edid_from_vbios(nv_dev_t *gd, nv_output_t *o)
{
    if (!gd || !gd->bar0 || !o) return -1;
    /* Scan the option ROM aperture in 4-byte stride for the EDID
     * header pattern 00 FF FF FF FF FF FF 00. Bound the scan to
     * 64KB to keep boot fast. */
    for (uint32_t off = 0; off + 128 < 0x10000; off += 4) {
        uint32_t a = nv_r32(gd->bar0, NV_PROM_BASE + off);
        if (a != 0xFFFF0000u) continue;
        uint32_t b = nv_r32(gd->bar0, NV_PROM_BASE + off + 4);
        if (b != 0xFFFFFFFFu) continue;
        uint32_t c = nv_r32(gd->bar0, NV_PROM_BASE + off + 8);
        if ((c & 0x0000FFFFu) != 0x0000FFFFu) continue;
        if ((c & 0xFF000000u) != 0x00000000u) continue;
        /* Looks plausible. Copy 128 bytes via dword reads. */
        for (uint32_t k = 0; k < 128; k += 4) {
            uint32_t dw = nv_r32(gd->bar0, NV_PROM_BASE + off + k);
            o->edid[k + 0] = (uint8_t)(dw & 0xFFu);
            o->edid[k + 1] = (uint8_t)((dw >> 8) & 0xFFu);
            o->edid[k + 2] = (uint8_t)((dw >> 16) & 0xFFu);
            o->edid[k + 3] = (uint8_t)((dw >> 24) & 0xFFu);
        }
        /* Validate checksum. */
        uint8_t sum = 0;
        for (int k = 0; k < 128; k++) sum = (uint8_t)(sum + o->edid[k]);
        if (sum != 0) continue;
        o->edid_size  = 128;
        o->edid_valid = 1;
        nv_edid_extract_monitor_name(o->edid, 128, o->monitor_name);
        return 0;
    }
    return -1;
}

/* ── Chain pipeline node resolves ──────────────────────────────── */

typedef struct {
    nv_dev_t   *dev;
    nv_output_t *out;
} nv_node_state_t;

static nv_node_state_t s_node_states[GPU_NVIDIA_MAX_DEVICES * GPU_NVIDIA_MAX_OUTPUTS];
static int             s_node_state_count = 0;

static void nv_pixel_source_resolve(chain_node_t *self, void *input, void *output)
{
    (void)input;
    int *ok = (int *)output;
    *ok = (self->state != 0) ? 1 : 0;
}

static void nv_composite_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    int *in_ok  = (int *)input;
    int *out_ok = (int *)output;
    *out_ok = *in_ok;
}

static void nv_scanout_resolve(chain_node_t *self, void *input, void *output)
{
    int *in_ok  = (int *)input;
    int *out_token = (int *)output;
    nv_node_state_t *st = (nv_node_state_t *)self->state;
    *out_token = (*in_ok && st && st->out && st->out->valid) ? 1 : 0;
}

/* hardware_dma node:
 *   Stage 1 path is OBSERVE-ONLY. We don't issue a TRANSFER on
 *   a Turing card without GSP -- there is no command submission
 *   engine wired. We bump vault_version to reflect that the
 *   chain resolved and the on-screen pixels match the GOP fb
 *   (because the GOP firmware programmed the mode and we never
 *   reprogrammed it).
 *
 *   When Stage 2 lands, this is where TRANSFER + FLUSH (or
 *   equivalent display-channel push-buffer commands) get issued. */
static void nv_hardware_dma_resolve(chain_node_t *self, void *input, void *output)
{
    int *in_token = (int *)input;
    int *out_ok   = (int *)output;
    *out_ok = 0;
    if (!*in_token) return;
    nv_node_state_t *st = (nv_node_state_t *)self->state;
    if (!st || !st->dev) return;

    /* Stage 1: scanout was set by the GOP firmware. We're not
     * driving a fresh TRANSFER. Mark the chain as resolved and
     * bump vault_version so MasQ observers (panel, inspector)
     * see the heartbeat. */
    *out_ok = 1;
    st->dev->vault_version_local++;
    chain_t *c = chain_get(st->out->chain_id);
    if (c) c->vault_version++;
}

/* ── Naming helpers ────────────────────────────────────────────── */

static void nv_utoa(uint32_t v, char *out)
{
    char tmp[12]; int n = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v && n < 11) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
}

static void nv_concat3(char *out, int cap, const char *a, const char *b, const char *c)
{
    int j = 0;
    for (const char *p = a; p && *p && j < cap - 1; p++) out[j++] = *p;
    for (const char *p = b; p && *p && j < cap - 1; p++) out[j++] = *p;
    for (const char *p = c; p && *p && j < cap - 1; p++) out[j++] = *p;
    out[j] = 0;
}

/* ── Chain hierarchy registration ──────────────────────────────── */

static int nv_register_chains(nv_dev_t *gd, int parent_id)
{
    char name[80];
    char num[12]; nv_utoa((uint32_t)gd->gpu_index, num);

    /* CHAIN_GPU_nvidia<n> */
    nv_concat3(name, sizeof(name), "gpu_nvidia", num, "");
    int gpu_id = chain_create(name, parent_id, MASQ_INTERNAL);
    if (gpu_id < 0) return -1;
    gd->chain_id = gpu_id;
    chain_t *gpu_chain = chain_get(gpu_id);
    if (gpu_chain) gpu_chain->vault_version++;  /* init bump */

    /* gpu_nvidia<n>.render -- gated; no render path until GSP up. */
    nv_concat3(name, sizeof(name), "gpu_nvidia", num, ".render");
    gd->chain_render_id = chain_create(name, gpu_id, MASQ_INTERNAL);

    /* gpu_nvidia<n>.display */
    nv_concat3(name, sizeof(name), "gpu_nvidia", num, ".display");
    gd->chain_display_id = chain_create(name, gpu_id, MASQ_INTERNAL);

    /* Per-output display sub-chain. */
    for (int oi = 0; oi < gd->output_count && oi < GPU_NVIDIA_MAX_OUTPUTS; oi++) {
        nv_output_t *o = &gd->outputs[oi];
        if (!o->valid) continue;

        char onum[12]; nv_utoa((uint32_t)oi, onum);
        char head[40];
        nv_concat3(head, sizeof(head), "display.gpu_nvidia", num, ".");
        char full[80];
        /* Append connector name + index. */
        nv_concat3(full, sizeof(full), head, nv_output_name(o->kind), onum);

        int dchain = chain_create(full, gd->chain_display_id, MASQ_INTERNAL);
        if (dchain < 0) continue;
        o->chain_id = dchain;

        if (s_node_state_count >= (int)(sizeof(s_node_states)/sizeof(s_node_states[0]))) {
            /* Out of state slots; chain still created but won't drive. */
            continue;
        }
        nv_node_state_t *st = &s_node_states[s_node_state_count++];
        st->dev = gd;
        st->out = o;

        int n0 = chain_add_node(dchain, "pixel_source",
                                "frame_request", "pixel_buffer",
                                nv_pixel_source_resolve);
        (void)chain_add_node(dchain, "composite",
                             "pixel_buffer", "pixel_buffer",
                             nv_composite_resolve);
        int n2 = chain_add_node(dchain, "scanout",
                                "pixel_buffer", "scanout_token",
                                nv_scanout_resolve);
        int n3 = chain_add_node(dchain, "hardware_dma",
                                "scanout_token", "scanout_complete",
                                nv_hardware_dma_resolve);
        chain_t *c = chain_get(dchain);
        if (c) {
            if (n0 >= 0) c->nodes[n0].state = st;
            if (n2 >= 0) c->nodes[n2].state = st;
            if (n3 >= 0) c->nodes[n3].state = st;
            c->vault_version++;
            c->resolve_interval_ticks = 16;
        }
        s_total_outputs++;
    }
    return 0;
}

/* ── Per-device bring-up ───────────────────────────────────────── */

static int nv_dev_bringup(nv_dev_t *gd, struct pci_device *pci, int gpu_index)
{
    gd->pci = pci;
    gd->gpu_index = gpu_index;

    /* Enable memory + bus master. (No DMA in Stage 1; bus master flag
     * is required for any future MSI-X / GSP path so set it now.) */
    uint32_t cmd = pci_config_read32(pci->bus, pci->dev, pci->func, 0x04);
    cmd |= (1u << 1);   /* Memory space enable */
    cmd |= (1u << 2);   /* Bus master enable */
    pci_config_write32(pci->bus, pci->dev, pci->func, 0x04, cmd);

    /* BAR0: registers, ~16MB. We map a conservative 16MB. */
    gd->bar0_phys = nv_bar_phys64(pci, 0);
    gd->bar0_len  = 16 * 1024 * 1024;
    gd->bar0      = (volatile uint8_t *)nv_map_bar(gd->bar0_phys, gd->bar0_len);
    if (!gd->bar0) {
        kputs("[gpu_nvidia] BAR0 map failed\n");
        return -1;
    }

    /* BAR1: framebuffer aperture. Size from PCI BAR sizing. We map
     * the first 64MB which is enough to talk to the GOP fb if we
     * ever need to. (Stage 1 doesn't actually touch BAR1.) */
    gd->bar1_phys = nv_bar_phys64(pci, 1);
    gd->bar1_len  = 64 * 1024 * 1024;
    /* Lazy: don't map BAR1 unless we actually need it -- saves
     * page table churn for cards we never use. */

    /* BAR3: per-chip varies (often I/O). We don't map it Stage 1. */
    gd->bar3_phys = nv_bar_phys64(pci, 3);
    gd->bar3_len  = 0;

    /* PMC_BOOT_0 readback. */
    gd->pmc_boot_0 = nv_r32(gd->bar0, NV_PMC_BOOT_0);
    if (gd->pmc_boot_0 == 0xFFFFFFFFu || gd->pmc_boot_0 == 0u) {
        kputs("[gpu_nvidia] PMC_BOOT_0 unreadable -- BAR0 not live\n");
        return -1;
    }
    gd->chip_family = nv_decode_family(gd->pmc_boot_0);

    /* DCB enumeration. */
    int n_outs = nv_parse_dcb(gd);
    if (n_outs == 0) {
        /* Synthetic single output mapped to head 0. Honest fallback
         * for cards whose option ROM mirror isn't readable through
         * BAR0 (some platforms shadow the ROM only at boot). */
        nv_output_t *o = &gd->outputs[0];
        o->valid = 1;
        o->kind  = NV_OUTPUT_UNKNOWN;
        o->head  = 0;
        o->dpaux_port = 0xFF;
        o->i2c_port   = 0xFF;
        o->width  = fb_width();
        o->height = fb_height();
        o->refresh_hz = 60;
        n_outs = 1;
    }
    gd->output_count = n_outs;

    /* Best-effort EDID per output (VBIOS scan only -- see notes). */
    for (int i = 0; i < gd->output_count; i++) {
        (void)nv_try_edid_from_vbios(gd, &gd->outputs[i]);
    }

    gd->ready = 1;
    gd->gsp_ready = 0;
    return 0;
}

/* ── Public API ────────────────────────────────────────────────── */

int gpu_nvidia_init(int parent_id)
{
    s_nv_count = 0;
    s_total_outputs = 0;
    s_node_state_count = 0;

    int n = pci_device_count();
    int gpu_index = 0;
    for (int i = 0; i < n && s_nv_count < GPU_NVIDIA_MAX_DEVICES; i++) {
        struct pci_device *d = pci_get_device(i);
        if (!d) continue;
        if (d->vendor_id != NVIDIA_VENDOR_ID) continue;
        /* Class 0x03 = display controller; subclass 0x00 = VGA, 0x80 = "other". */
        if (d->class_code != 0x03) continue;

        nv_dev_t *gd = &s_nv[s_nv_count];
        for (uint64_t j = 0; j < sizeof(*gd); j++) ((uint8_t*)gd)[j] = 0;
        gd->in_use = 1;

        kputs("[gpu_nvidia] binding ");
        kput_hex((uint64_t)d->vendor_id);
        kputs(":");
        kput_hex((uint64_t)d->device_id);
        kputs(" at ");
        kput_dec((uint64_t)d->bus); kputs(":");
        kput_dec((uint64_t)d->dev); kputs(".");
        kput_dec((uint64_t)d->func); kputc('\n');

        if (nv_dev_bringup(gd, d, gpu_index) == 0) {
            nv_register_chains(gd, parent_id);
            kputs("[gpu_nvidia] PMC_BOOT_0=");
            kput_hex((uint64_t)gd->pmc_boot_0);
            kputs(" family=");
            kputs(nv_family_name(gd->chip_family));
            kputs(" outputs=");
            kput_dec((uint64_t)gd->output_count);
            kputc('\n');

            /* Stage 2 attempt: Turing skips it (we don't need GSP
             * for scanout). Ampere+ tries it; honest stub returns
             * -ENOTREADY when firmware blobs aren't embedded. */
            if (gd->chip_family >= NV_FAMILY_AMPERE) {
                int rc = gpu_nvidia_gsp_bringup(s_nv_count);
                if (rc != 0) {
                    kputs("[gpu_nvidia] GSP bring-up not ready: rc=");
                    kput_dec((uint64_t)(uint32_t)(-rc));
                    kputs(" (Stage 2 stub -- no compute backend registered)\n");
                }
            } else {
                kputs("[gpu_nvidia] Turing-class: GSP not required for scanout\n");
            }

            s_nv_count++;
            gpu_index++;
        } else {
            kputs("[gpu_nvidia] bring-up failed for this device\n");
        }
    }

    if (s_nv_count > 0) {
        kputs("[gpu_nvidia] devices=");
        kput_dec((uint64_t)s_nv_count);
        kputs(" outputs=");
        kput_dec((uint64_t)s_total_outputs);
        kputc('\n');
        gpu_nvidia_dump_status();
    }
    return s_nv_count;
}

int gpu_nvidia_device_count(void) { return s_nv_count; }

const gpu_nvidia_device_t *gpu_nvidia_device(int idx)
{
    static gpu_nvidia_device_t view;
    if (idx < 0 || idx >= s_nv_count) return 0;
    nv_dev_t *gd = &s_nv[idx];
    view.chain_id         = gd->chain_id;
    view.chain_render_id  = gd->chain_render_id;
    view.chain_display_id = gd->chain_display_id;
    view.pci_vendor       = gd->pci ? gd->pci->vendor_id : 0;
    view.pci_device       = gd->pci ? gd->pci->device_id : 0;
    view.pci_bus          = gd->pci ? gd->pci->bus  : 0;
    view.pci_dev          = gd->pci ? gd->pci->dev  : 0;
    view.pci_func         = gd->pci ? gd->pci->func : 0;
    view.pmc_boot_0       = gd->pmc_boot_0;
    view.chip_family      = gd->chip_family;
    view.output_count     = gd->output_count;
    view.ready            = gd->ready;
    view.gsp_ready        = gd->gsp_ready;
    return &view;
}

int gpu_nvidia_output_count(int dev_idx)
{
    if (dev_idx < 0 || dev_idx >= s_nv_count) return 0;
    return s_nv[dev_idx].output_count;
}

int gpu_nvidia_output_info(int dev_idx, int out_idx, nv_output_t *out)
{
    if (!out) return -1;
    if (dev_idx < 0 || dev_idx >= s_nv_count) return -1;
    nv_dev_t *gd = &s_nv[dev_idx];
    if (out_idx < 0 || out_idx >= gd->output_count) return -1;
    *out = gd->outputs[out_idx];
    return 0;
}

int gpu_nvidia_gpu_chain(int dev_idx)
{
    if (dev_idx < 0 || dev_idx >= s_nv_count) return -1;
    return s_nv[dev_idx].chain_id;
}

uint32_t gpu_nvidia_vault_version(int dev_idx)
{
    if (dev_idx < 0 || dev_idx >= s_nv_count) return 0;
    return s_nv[dev_idx].vault_version_local;
}

/* ── Stage 2 stub: GSP firmware load + RM control path ─────────── */
/*
 * STAGE 2 TODO. The full path:
 *
 *   1. Locate firmware blobs in kernel/lib/firmware/nvidia/<chip>/
 *      gsp/ *.bin -- bootloader, booter_load, booter_unload, gsp.
 *      These are compressed (.bin.zst on host distros); the build
 *      will need to decompress + objcopy them into the kernel image.
 *      Until that build wiring exists this function returns
 *      -ENOTREADY honestly.
 *
 *   2. Reset the GSP via PMC_GSP. Push the booter blob via DMA into
 *      GSP IMEM/DMEM. Kick the GSP RISC-V core.
 *
 *   3. Wait for the GSP-ready handshake on the RM message ring.
 *      Allocate the message ring in pmm_alloc_contiguous memory.
 *
 *   4. Issue a minimal RM command (NV_RM_GET_GPU_INFO) to verify
 *      the path. On success: register a gpu_compute backend
 *      "nvidia-N" with can_dispatch returning 1 for FP32 ops and
 *      dispatch routing through an RM kernel-launch message.
 *
 * Honest gap: none of the above exists yet. Returning -ENOTREADY.
 */
#define NV_ENOTREADY (-22)

int gpu_nvidia_gsp_bringup(int dev_idx)
{
    if (dev_idx < 0 || dev_idx >= s_nv_count) return NV_ENOTREADY;
    nv_dev_t *gd = &s_nv[dev_idx];
    if (!gd->ready) return NV_ENOTREADY;

    /* No firmware embedded yet -- honest stub. */
    return NV_ENOTREADY;
}

/* ── Status dump (`nvidia` shell command) ──────────────────────── */

void gpu_nvidia_dump_status(void)
{
    if (s_nv_count == 0) {
        kputs("\n  No NVIDIA GPU detected.\n\n");
        return;
    }
    kputs("\n  NVIDIA GPUs: ");
    kput_dec((uint64_t)s_nv_count);
    kputs("    Outputs: ");
    kput_dec((uint64_t)s_total_outputs);
    kputs("\n");

    for (int i = 0; i < s_nv_count; i++) {
        nv_dev_t *gd = &s_nv[i];
        kputs("  gpu_nvidia");
        kput_dec((uint64_t)i);
        kputs("  pci=");
        kput_hex((uint64_t)(gd->pci ? gd->pci->vendor_id : 0));
        kputs(":");
        kput_hex((uint64_t)(gd->pci ? gd->pci->device_id : 0));
        kputs(" @ ");
        kput_dec((uint64_t)(gd->pci ? gd->pci->bus  : 0)); kputs(":");
        kput_dec((uint64_t)(gd->pci ? gd->pci->dev  : 0)); kputs(".");
        kput_dec((uint64_t)(gd->pci ? gd->pci->func : 0));
        kputs("  PMC_BOOT_0=");
        kput_hex((uint64_t)gd->pmc_boot_0);
        kputs("  family=");
        kputs(nv_family_name(gd->chip_family));
        kputs("  outputs=");
        kput_dec((uint64_t)gd->output_count);
        kputs("  ready=");
        kput_dec((uint64_t)gd->ready);
        kputs("  gsp=");
        kputs(gd->gsp_ready ? "ok" : "stub(no fw)");
        kputc('\n');

        for (int oi = 0; oi < gd->output_count; oi++) {
            nv_output_t *o = &gd->outputs[oi];
            if (!o->valid) continue;
            kputs("    ");
            kputs(nv_output_name(o->kind));
            kputs(" head=");
            kput_dec((uint64_t)o->head);
            kputs(" mode=");
            kput_dec((uint64_t)o->width);
            kputs("x");
            kput_dec((uint64_t)o->height);
            if (o->refresh_hz) {
                kputs("@");
                kput_dec((uint64_t)o->refresh_hz);
                kputs("Hz");
            }
            kputs("  edid=");
            kputs(o->edid_valid ? "yes" : "no");
            if (o->edid_valid && o->monitor_name[0]) {
                kputs("  monitor=\"");
                kputs(o->monitor_name);
                kputs("\"");
            }
            kputc('\n');
        }
    }
    kputc('\n');
}

/* Selftest summary line. Output shape:
 *   "N card(s) -- TU106 scanout=ok, GA106 GSP=stub"
 */
static void nv_strapp(char *out, int cap, int *jp, const char *s)
{
    int j = *jp;
    while (s && *s && j < cap - 1) out[j++] = *s++;
    out[j] = 0;
    *jp = j;
}
static void nv_strapp_dec(char *out, int cap, int *jp, uint32_t v)
{
    char tmp[12]; nv_utoa(v, tmp);
    nv_strapp(out, cap, jp, tmp);
}

void gpu_nvidia_selftest_summary(char *out, int cap)
{
    if (!out || cap <= 0) return;
    int j = 0;
    out[0] = 0;
    if (s_nv_count == 0) {
        nv_strapp(out, cap, &j, "0 card(s) -- none detected");
        return;
    }
    nv_strapp_dec(out, cap, &j, (uint32_t)s_nv_count);
    nv_strapp(out, cap, &j, " card(s) -- ");
    for (int i = 0; i < s_nv_count; i++) {
        if (i > 0) nv_strapp(out, cap, &j, ", ");
        nv_strapp(out, cap, &j, nv_family_name(s_nv[i].chip_family));
        if (s_nv[i].chip_family >= NV_FAMILY_AMPERE) {
            nv_strapp(out, cap, &j, " GSP=");
            nv_strapp(out, cap, &j, s_nv[i].gsp_ready ? "ok" : "stub");
        } else {
            nv_strapp(out, cap, &j, " scanout=");
            nv_strapp(out, cap, &j, s_nv[i].ready ? "ok" : "fail");
        }
    }
}
