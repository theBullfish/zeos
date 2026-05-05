/*
 * Zeos -- Intel HD Audio (HDA) driver
 *
 * Chain-native HDA driver. Pipeline:
 *   pcm_source -> volume_filter -> hda_pin -> hardware_dma
 * Each node is a chain_node_t resolved through chain_resolve().
 * MasQ tier: INTERNAL. Parent: CHAIN_CPU.
 * This is the first hardware driver converted to the Zeos native paradigm
 * (per docs/PARADIGM_CONVERSION.md). NIC, NVMe, AHCI, USB follow this shape.
 *
 * Spec: Intel HD Audio Specification rev 1.0a (2010).
 *
 * Minimum-viable: PCI class 0x04 / subclass 0x03, MMIO at BAR0, CORB/RIRB
 * DMA rings for codec verbs, one BDL per output stream. Walks the codec
 * tree, finds an analog output pin, configures SD0 for 48 kHz/16-bit
 * stereo, plays PCM. Polling-only -- no MSI/IRQ. 48 kHz only.
 *
 * Imperative bringup (PCI scan, controller reset, CORB/RIRB ring setup,
 * codec walk, stream pre-config) still runs in hda_init() so the controller
 * is live before chain_registry_init() registers CHAIN_AUDIO. After that,
 * every PCM playback flows through chain_resolve(CHAIN_AUDIO). hda_play_pcm
 * is a compat shim that stages a pcm_request and calls chain_resolve.
 */

#include "hda.h"
#include "pci.h"
#include "pmm.h"
#include "kprint.h"
#include "timer.h"
#include "chain.h"
#include "chain_registry.h"
#include "vault.h"
#include "spinlock.h"

#include <stdint.h>
#include <stddef.h>

/* ── Master volume / mute state ────────────────────────────────────
 *
 * g_audio is the single source of truth for the master output level.
 * The shell, F-keys, and persistence layer all read/write through the
 * hda_audio_* API; they never touch this struct directly. The amps are
 * pushed by hda_audio_apply_amps() which writes verb 0x300 to the DAC
 * and pin output amps for both channels.
 *
 * Default 60/100, unmuted, until hda_audio_restore_from_vault() loads
 * a previous value from /audio/volume.
 *
 * On-disk format is the raw hda_audio_persist_t (8 bytes). magic
 * guards against a corrupt or short read.
 */

#define AUDIO_VAULT_PATH    "/audio/volume"
#define AUDIO_VAULT_MAGIC   0x5A415544u    /* 'ZAUD' */
#define AUDIO_VAULT_VERSION 1u

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  volume;     /* 0..100 */
    uint8_t  muted;      /* 0 or 1 */
    uint8_t  reserved;
} hda_audio_persist_t;

/* g_audio used to be a free-floating struct here; per CHAIN_CONTRACT
 * it's now hosted on CHAIN_AUDIO's volume_filter node `state` pointer.
 * The volume_filter_state_t struct (see below) holds {volume, muted,
 * initialized} as its first three fields. We expose g_audio as an
 * alias view through a helper that resolves the node state, so the
 * existing apply_amps / persistence / fallback paths continue to work
 * unchanged while every read AND write goes through the chain node. */
static int *audio_volume_ptr(void);
static int *audio_muted_ptr(void);
static int *audio_initialized_ptr(void);
#define G_AUDIO_VOLUME       (*audio_volume_ptr())
#define G_AUDIO_MUTED        (*audio_muted_ptr())
#define G_AUDIO_INITIALIZED  (*audio_initialized_ptr())

#define HDA_GCAP       0x00
#define HDA_GCTL       0x08
#define HDA_STATESTS   0x0E
#define HDA_CORBLBASE  0x40
#define HDA_CORBUBASE  0x44
#define HDA_CORBWP     0x48
#define HDA_CORBRP     0x4A
#define HDA_CORBCTL    0x4C
#define HDA_CORBSIZE   0x4E
#define HDA_RIRBLBASE  0x50
#define HDA_RIRBUBASE  0x54
#define HDA_RIRBWP     0x58
#define HDA_RINTCNT    0x5A
#define HDA_RIRBCTL    0x5C
#define HDA_RIRBSIZE   0x5E

#define GCTL_CRST      (1u << 0)
#define CORBCTL_RUN    (1u << 1)
#define RIRBCTL_RUN    (1u << 1)
#define CORBRP_RST     (1u << 15)

#define SD_CTL         0x00
#define SD_STS         0x03
#define SD_CBL         0x08
#define SD_LVI         0x0C
#define SD_FMT         0x12
#define SD_BDPL        0x18
#define SD_BDPU        0x1C

#define SDCTL_RUN      (1u << 1)
#define SDCTL_SRST     (1u << 0)

#define VERB_GET_PARAM             0xF00
#define VERB_GET_CONFIG_DEFAULT    0xF1C
#define VERB_GET_CONNECT_LIST      0xF02
#define VERB_GET_CONV_STREAM_CHAN  0xF06
#define VERB_GET_PIN_CTL           0xF07
#define VERB_SET_POWER             0x705
#define VERB_SET_PIN_CTL           0x707
#define VERB_SET_CONV_FMT          0x200
#define VERB_SET_CONV_STREAM_CHAN  0x706
#define VERB_SET_AMP_GAIN          0x300
#define VERB_SET_EAPD              0x70C
#define VERB_SET_CONNECT_SEL       0x701

#define PARAM_VENDOR_ID            0x00
#define PARAM_NODE_COUNT           0x04
#define PARAM_FUNC_GROUP_TYPE      0x05
#define PARAM_WIDGET_CAP           0x09
#define PARAM_PIN_CAP              0x0C
#define PARAM_CONNLIST_LEN         0x0E

#define WT_AUDIO_OUTPUT    0x0
#define WT_AUDIO_INPUT     0x1
#define WT_AUDIO_MIXER     0x2
#define WT_AUDIO_SELECTOR  0x3
#define WT_PIN_COMPLEX     0x4

#define WCAP_CONN_LIST     (1u << 8)
#define WCAP_OUT_AMP       (1u << 2)
#define WCAP_IN_AMP        (1u << 1)

#define PIN_CAP_OUTPUT      (1u << 4)
#define PIN_CAP_HEADPHONE   (1u << 3)
#define PIN_CAP_EAPD        (1u << 16)
#define PIN_CTL_OUT_ENABLE  (1u << 6)
#define PIN_CTL_HP_ENABLE   (1u << 7)

#define MAX_PATH_NIDS      8

static volatile uint8_t  *mmio = 0;
static int                ready = 0;
static const char        *status_msg = "not initialized";

static uint32_t          *corb;
static uint64_t          *rirb;
static uint64_t           ring_phys;
static uint16_t           rirb_rp;

/* SMP — codec verb send/wait must be serialized. CORB write index +
 * RIRB read pointer are shared global state; two cores calling
 * codec_cmd concurrently would interleave verbs and read each other's
 * responses. The volume_filter resolve and any audio.beep both walk
 * through codec_cmd, so this lock is the real exposure point. */
static zeos_spinlock_t    s_codec_lock = ZEOS_SPINLOCK_INIT;

struct bdl_entry { uint32_t addr_lo, addr_hi, length, flags; };

#define HDA_AUDIO_BYTES   (64 * 1024)
#define HDA_AUDIO_PAGES   (HDA_AUDIO_BYTES / 4096)

static struct bdl_entry  *bdl;
static uint64_t           bdl_phys;
static uint8_t           *audio_buf;
static uint64_t           audio_buf_phys;

static uint8_t            codec_addr = 0xFF;
static uint16_t           dac_nid    = 0;
static uint16_t           pin_nid    = 0;
static uint16_t           path_nids[MAX_PATH_NIDS];   /* DAC..pin, inclusive */
static uint8_t            path_len   = 0;

static int                out_sd_idx = -1;
static volatile uint8_t  *sd_regs = 0;

/* ── Chain-node-private state ──────────────────────────────────────── */

typedef struct {
    /* The active request, staged by hda_play_pcm() before chain_resolve. */
    hda_pcm_request_t pending;
} pcm_source_state_t;

typedef struct {
    /* Canonical master volume / mute state (CHAIN_CONTRACT: lives on the
     * chain node `state` pointer, not as a free static). */
    int volume;          /* 0..100 */
    int muted;           /* 0/1 */
    int initialized;     /* set once we have a codec path */

    /* 0..256: 256 = unity, 0 = mute. Linear scale, fixed-point /256. */
    int gain_q8;

    /* When non-zero, the next chain_resolve(CHAIN_AUDIO) commits the
     * staged volume/mute change: applies to hardware via apply_amps,
     * bumps vault_version, persists to VAULT. Set by hda_audio_set_*
     * which then calls chain_resolve to drive the change through the
     * pipeline rather than mutating state directly. */
    int pending_change;
    int pending_volume;
    int pending_muted;
} volume_filter_state_t;

typedef struct {
    /* Codec/pin we're driving + last gain we pushed. -1 = not yet pushed. */
    uint8_t  cad;
    uint16_t dac;
    uint16_t pin;
    int      pushed;     /* 1 once codec pin/amp settings have been applied */
} hda_pin_state_t;

typedef struct {
    /* SD0 register pointers + BDL phys -- already resolved in hda_init. */
    volatile uint8_t *sd_regs;
    uint64_t          bdl_phys;
} hda_dma_state_t;

static pcm_source_state_t    s_pcm_source;
static volume_filter_state_t s_volume_filter = {
    .volume = 60, .muted = 0, .initialized = 0, .gain_q8 = 256,
    .pending_change = 0, .pending_volume = 0, .pending_muted = 0,
};
static hda_pin_state_t       s_hda_pin;
static hda_dma_state_t       s_hda_dma;

static int *audio_volume_ptr(void)      { return &s_volume_filter.volume; }
static int *audio_muted_ptr(void)       { return &s_volume_filter.muted; }
static int *audio_initialized_ptr(void) { return &s_volume_filter.initialized; }

static inline uint8_t  mr8 (uint32_t off) { return *(volatile uint8_t  *)(mmio + off); }
static inline uint16_t mr16(uint32_t off) { return *(volatile uint16_t *)(mmio + off); }
static inline uint32_t mr32(uint32_t off) { return *(volatile uint32_t *)(mmio + off); }
static inline void     mw8 (uint32_t off, uint8_t  v) { *(volatile uint8_t  *)(mmio + off) = v; }
static inline void     mw16(uint32_t off, uint16_t v) { *(volatile uint16_t *)(mmio + off) = v; }
static inline void     mw32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(mmio + off) = v; }

static inline uint8_t  sdr8_at (volatile uint8_t *r, uint32_t off) { return *(volatile uint8_t  *)(r + off); }
static inline void     sdw8_at (volatile uint8_t *r, uint32_t off, uint8_t  v) { *(volatile uint8_t  *)(r + off) = v; }
static inline void     sdw16_at(volatile uint8_t *r, uint32_t off, uint16_t v) { *(volatile uint16_t *)(r + off) = v; }
static inline void     sdw32_at(volatile uint8_t *r, uint32_t off, uint32_t v) { *(volatile uint32_t *)(r + off) = v; }

static inline uint8_t  sdr8 (uint32_t off) { return sdr8_at(sd_regs, off); }
static inline void     sdw8 (uint32_t off, uint8_t  v) { sdw8_at(sd_regs, off, v); }
static inline void     sdw16(uint32_t off, uint16_t v) { sdw16_at(sd_regs, off, v); }
static inline void     sdw32(uint32_t off, uint32_t v) { sdw32_at(sd_regs, off, v); }

static void busy_pause(int loops)
{
    for (volatile int i = 0; i < loops; i++) __asm__ volatile("pause");
}

static uint32_t make_verb(uint8_t cad, uint16_t nid, uint16_t verb, uint16_t payload)
{
    /* HDA spec 7.3.1:
     *   short form (4-bit verb id + 16-bit data) is used when the verb
     *   number is in the range 0x000..0x3FF (top nibble 0..3).
     *   long form  (12-bit verb id + 8-bit data) is used otherwise.
     * The previous classifier matched (verb & 0xF00) == 0x700, which
     * incorrectly treated 0x70x verbs (Set Power, Set Pin Ctl, Set
     * Stream/Channel, Set EAPD) as short form, scrambling configuration.
     */
    uint32_t v = ((uint32_t)cad & 0xF) << 28;
    v |= ((uint32_t)nid & 0xFF) << 20;
    if (verb < 0x400) {
        /* short form: top nibble of verb in [19:16], 16-bit data in [15:0] */
        v |= ((uint32_t)(verb & 0xF00)) << 8;
        v |= (payload & 0xFFFF);
    } else {
        /* long form: 12-bit verb in [19:8], 8-bit data in [7:0] */
        v |= ((uint32_t)(verb & 0xFFF)) << 8;
        v |= (payload & 0xFF);
    }
    return v;
}

static uint32_t codec_cmd(uint8_t cad, uint16_t nid, uint16_t verb, uint16_t payload)
{
    uint64_t flags = spin_lock_irqsave(&s_codec_lock);
    uint32_t cmd = make_verb(cad, nid, verb, payload);
    uint16_t wp = mr16(HDA_CORBWP) & 0xFF;
    uint16_t next = (wp + 1) & 0xFF;
    corb[next] = cmd;
    __asm__ volatile("" ::: "memory");
    mw16(HDA_CORBWP, next);

    for (int i = 0; i < 100000; i++) {
        uint16_t rwp = mr16(HDA_RIRBWP) & 0xFF;
        if (rwp != rirb_rp) {
            rirb_rp = (rirb_rp + 1) & 0xFF;
            uint64_t r = rirb[rirb_rp];
            spin_unlock_irqrestore(&s_codec_lock, flags);
            return (uint32_t)(r & 0xFFFFFFFFu);
        }
        busy_pause(100);
    }
    spin_unlock_irqrestore(&s_codec_lock, flags);
    return 0xFFFFFFFFu;
}

static int hda_reset(void)
{
    mw32(HDA_GCTL, mr32(HDA_GCTL) & ~GCTL_CRST);
    for (int i = 0; i < 1000; i++) {
        if (!(mr32(HDA_GCTL) & GCTL_CRST)) break;
        busy_pause(1000);
    }
    mw32(HDA_GCTL, mr32(HDA_GCTL) | GCTL_CRST);
    for (int i = 0; i < 1000; i++) {
        if (mr32(HDA_GCTL) & GCTL_CRST) {
            timer_wait_ms(1);
            return 0;
        }
        busy_pause(1000);
    }
    return -1;
}

static int hda_setup_corb_rirb(void)
{
    uint64_t phys = pmm_alloc_contiguous(1);
    if (!phys) { status_msg = "ring alloc failed"; return -1; }
    ring_phys = phys;
    uint8_t *page = (uint8_t *)(uintptr_t)phys;
    for (int i = 0; i < 4096; i++) page[i] = 0;

    corb = (uint32_t *)(page + 0);
    rirb = (uint64_t *)(page + 1024);

    mw8(HDA_CORBCTL, 0);
    mw8(HDA_RIRBCTL, 0);
    for (int i = 0; i < 100; i++) {
        if (!(mr8(HDA_CORBCTL) & CORBCTL_RUN) &&
            !(mr8(HDA_RIRBCTL) & RIRBCTL_RUN)) break;
        busy_pause(1000);
    }

    mw8(HDA_CORBSIZE, 0x02);
    mw8(HDA_RIRBSIZE, 0x02);

    mw32(HDA_CORBLBASE, (uint32_t)(ring_phys & 0xFFFFFFFFu));
    mw32(HDA_CORBUBASE, (uint32_t)(ring_phys >> 32));
    mw32(HDA_RIRBLBASE, (uint32_t)((ring_phys + 1024) & 0xFFFFFFFFu));
    mw32(HDA_RIRBUBASE, (uint32_t)((ring_phys + 1024) >> 32));

    mw16(HDA_CORBRP, CORBRP_RST);
    for (int i = 0; i < 100; i++) {
        if (mr16(HDA_CORBRP) & CORBRP_RST) break;
        busy_pause(1000);
    }
    mw16(HDA_CORBRP, 0);
    for (int i = 0; i < 100; i++) {
        if (!(mr16(HDA_CORBRP) & CORBRP_RST)) break;
        busy_pause(1000);
    }
    mw16(HDA_CORBWP, 0);

    mw16(HDA_RIRBWP, (1u << 15));
    mw16(HDA_RINTCNT, 1);
    rirb_rp = 0;

    mw8(HDA_CORBCTL, CORBCTL_RUN);
    mw8(HDA_RIRBCTL, RIRBCTL_RUN);
    return 0;
}

/* ── Generic codec topology walk ───────────────────────────────────────
 *
 * Spec ref: HDA 1.0a sections 7.1.2 (widget topology) and 7.3 (verbs).
 *
 * Walk strategy (works for QEMU 1af4:0012 and standard Intel codecs):
 *
 *   1. Get sub-node count of root (NID 0). Each sub-node is a Function
 *      Group; only AFG (type 0x01) carries audio widgets.
 *   2. Power up the AFG (D0).
 *   3. Enumerate every widget in the AFG; classify by WIDGET_CAP bits
 *      [23:20]: 0=DAC, 1=ADC, 2=Mixer, 3=Selector, 4=Pin Complex.
 *   4. For every Pin Complex with PIN_CAP_OUTPUT, walk backwards through
 *      its CONNECTION_LIST (verb 0xF02), recursing through Mixers and
 *      Selectors, until we land on a DAC widget. Cache the full path
 *      (DAC..pin, inclusive) so the configuration step can unmute every
 *      amp along the way.
 *   5. Prefer pins whose CONFIG_DEFAULT default-device is line-out (0),
 *      speaker (1), or headphone (2) for cosmetic purposes, but accept
 *      any output-capable pin if that's the only option. QEMU's
 *      hda-output exposes default-device = speaker, so this matches.
 *
 * Returns 0 on success with dac_nid/pin_nid/path_nids populated. */

static int read_conn_entry(uint8_t cad, uint16_t nid, uint8_t idx,
                           int long_form, uint16_t *out)
{
    /* CONNECTION_LIST verb 0xF02 returns up to 4 short entries (1 byte
     * each) or 2 long entries (2 bytes each) per call, indexed by the
     * starting offset in the data byte. */
    uint8_t off = long_form ? (idx & ~1u) : (idx & ~3u);
    uint32_t r = codec_cmd(cad, nid, VERB_GET_CONNECT_LIST, off);
    if (r == 0xFFFFFFFFu) return -1;
    if (long_form) {
        uint8_t slot = idx - off;          /* 0 or 1 */
        *out = (uint16_t)((r >> (slot * 16)) & 0x7FFF);
    } else {
        uint8_t slot = idx - off;          /* 0..3 */
        *out = (uint16_t)((r >> (slot * 8)) & 0x7F);
    }
    return 0;
}

/* Recursive depth-first trace pin -> ... -> DAC. Path is built in
 * reverse (pin first, DAC last) and reversed by the caller. */
static int trace_to_dac(uint8_t cad, uint16_t nid,
                        uint16_t *path, uint8_t *plen, uint8_t depth)
{
    if (depth >= MAX_PATH_NIDS) return -1;

    uint32_t wcap = codec_cmd(cad, nid, VERB_GET_PARAM, PARAM_WIDGET_CAP);
    uint8_t  wt   = (wcap >> 20) & 0xF;

    if (wt == WT_AUDIO_OUTPUT) {
        path[depth] = nid;
        *plen = depth + 1;
        return 0;
    }

    if (wt != WT_AUDIO_MIXER && wt != WT_AUDIO_SELECTOR &&
        wt != WT_PIN_COMPLEX) {
        /* Dead end: ADC/Power/Volume Knob/Beep Gen never reach a DAC. */
        return -1;
    }

    if (!(wcap & WCAP_CONN_LIST)) return -1;

    uint32_t cl_len = codec_cmd(cad, nid, VERB_GET_PARAM, PARAM_CONNLIST_LEN);
    int long_form = (cl_len & (1u << 7)) ? 1 : 0;
    uint8_t n = cl_len & 0x7F;
    if (n == 0) return -1;

    path[depth] = nid;

    for (uint8_t i = 0; i < n; i++) {
        uint16_t src = 0;
        if (read_conn_entry(cad, nid, i, long_form, &src) < 0) continue;
        if (src == 0 || src == nid) continue;
        if (trace_to_dac(cad, src, path, plen, depth + 1) == 0) {
            /* If this widget is a Selector, lock its connection-select
             * to the index that reaches the DAC. (Mixer just sums.) */
            if (wt == WT_AUDIO_SELECTOR) {
                codec_cmd(cad, nid, VERB_SET_CONNECT_SEL, i);
            }
            return 0;
        }
    }
    return -1;
}

static int hda_walk_codec(uint8_t cad)
{
    uint32_t r = codec_cmd(cad, 0, VERB_GET_PARAM, PARAM_NODE_COUNT);
    if (r == 0xFFFFFFFFu) return -1;
    uint16_t fg_start = (r >> 16) & 0xFF;
    uint16_t fg_count = r & 0xFF;

    for (uint16_t fg = fg_start; fg < fg_start + fg_count; fg++) {
        uint32_t ftype = codec_cmd(cad, fg, VERB_GET_PARAM, PARAM_FUNC_GROUP_TYPE);
        if ((ftype & 0xFF) != 0x01) continue;          /* AFG only */

        codec_cmd(cad, fg, VERB_SET_POWER, 0x00);
        timer_wait_ms(1);

        uint32_t nc = codec_cmd(cad, fg, VERB_GET_PARAM, PARAM_NODE_COUNT);
        uint16_t w_start = (nc >> 16) & 0xFF;
        uint16_t w_count = nc & 0xFF;

        uint16_t best_pin = 0, fallback_pin = 0;
        uint16_t best_path[MAX_PATH_NIDS];
        uint8_t  best_plen = 0;
        uint16_t fb_path[MAX_PATH_NIDS];
        uint8_t  fb_plen = 0;

        for (uint16_t w = w_start; w < w_start + w_count; w++) {
            uint32_t wcap = codec_cmd(cad, w, VERB_GET_PARAM, PARAM_WIDGET_CAP);
            uint8_t  wt = (wcap >> 20) & 0xF;
            if (wt != WT_PIN_COMPLEX) continue;

            uint32_t pcap = codec_cmd(cad, w, VERB_GET_PARAM, PARAM_PIN_CAP);
            if (!(pcap & PIN_CAP_OUTPUT)) continue;

            /* Power up the pin so it'll respond to subsequent verbs. */
            codec_cmd(cad, w, VERB_SET_POWER, 0x00);

            uint16_t local_path[MAX_PATH_NIDS];
            uint8_t  local_plen = 0;
            if (trace_to_dac(cad, w, local_path, &local_plen, 0) != 0)
                continue;

            uint32_t cfg = codec_cmd(cad, w, VERB_GET_CONFIG_DEFAULT, 0);
            uint8_t dev = (cfg >> 20) & 0xF;
            int preferred = (dev == 0 || dev == 1 || dev == 2);

            if (preferred && !best_pin) {
                best_pin = w;
                best_plen = local_plen;
                for (uint8_t i = 0; i < local_plen; i++) best_path[i] = local_path[i];
            } else if (!fallback_pin) {
                fallback_pin = w;
                fb_plen = local_plen;
                for (uint8_t i = 0; i < local_plen; i++) fb_path[i] = local_path[i];
            }
        }

        uint16_t chosen = best_pin ? best_pin : fallback_pin;
        if (!chosen) continue;

        uint16_t *src = best_pin ? best_path : fb_path;
        uint8_t   sln = best_pin ? best_plen  : fb_plen;

        /* trace_to_dac stored path in reverse order (pin first, DAC last
         * at index sln-1). Flip to DAC-first for clarity. */
        path_len = sln;
        for (uint8_t i = 0; i < sln; i++) path_nids[i] = src[sln - 1 - i];
        dac_nid = path_nids[0];
        pin_nid = path_nids[sln - 1];
        return 0;
    }
    return -1;
}

#define FMT_48K_16B_STEREO  ((0u << 14) | (1u << 4) | 1u)

static int hda_setup_stream(void)
{
    uint16_t gcap = mr16(HDA_GCAP);
    uint8_t iss = (gcap >> 8) & 0xF;
    uint8_t oss = (gcap >> 12) & 0xF;
    if (oss == 0) { status_msg = "no output streams"; return -1; }
    out_sd_idx = iss;
    sd_regs = mmio + 0x80 + out_sd_idx * 0x20;

    uint64_t apages = pmm_alloc_contiguous(HDA_AUDIO_PAGES);
    if (!apages) { status_msg = "audio buf alloc failed"; return -1; }
    audio_buf_phys = apages;
    audio_buf = (uint8_t *)(uintptr_t)apages;
    for (uint32_t i = 0; i < HDA_AUDIO_BYTES; i++) audio_buf[i] = 0;

    bdl = (struct bdl_entry *)(((uint8_t *)(uintptr_t)ring_phys) + 3072);
    bdl_phys = ring_phys + 3072;
    bdl[0].addr_lo = (uint32_t)(audio_buf_phys & 0xFFFFFFFFu);
    bdl[0].addr_hi = (uint32_t)(audio_buf_phys >> 32);
    bdl[0].length  = HDA_AUDIO_BYTES / 2;
    bdl[0].flags   = 0;
    bdl[1].addr_lo = (uint32_t)((audio_buf_phys + HDA_AUDIO_BYTES/2) & 0xFFFFFFFFu);
    bdl[1].addr_hi = (uint32_t)((audio_buf_phys + HDA_AUDIO_BYTES/2) >> 32);
    bdl[1].length  = HDA_AUDIO_BYTES / 2;
    bdl[1].flags   = 0;

    sdw8(SD_CTL, SDCTL_SRST);
    for (int i = 0; i < 100; i++) {
        if (sdr8(SD_CTL) & SDCTL_SRST) break;
        busy_pause(1000);
    }
    sdw8(SD_CTL, 0);
    for (int i = 0; i < 100; i++) {
        if (!(sdr8(SD_CTL) & SDCTL_SRST)) break;
        busy_pause(1000);
    }

    sdw32(SD_BDPL, (uint32_t)(bdl_phys & 0xFFFFFFFFu));
    sdw32(SD_BDPU, (uint32_t)(bdl_phys >> 32));
    sdw32(SD_CBL,  HDA_AUDIO_BYTES);
    sdw16(SD_LVI,  1);
    sdw16(SD_FMT,  FMT_48K_16B_STEREO);

    uint8_t ctl2 = sdr8(SD_CTL + 2);
    ctl2 = (ctl2 & 0x0F) | (1 << 4);
    sdw8(SD_CTL + 2, ctl2);

    codec_cmd(codec_addr, dac_nid, VERB_SET_CONV_STREAM_CHAN, (1 << 4) | 0);
    codec_cmd(codec_addr, dac_nid, VERB_SET_CONV_FMT, FMT_48K_16B_STEREO);

    /* Power up every widget along the path. */
    for (uint8_t i = 0; i < path_len; i++) {
        codec_cmd(codec_addr, path_nids[i], VERB_SET_POWER, 0x00);
    }

    /* Unmute the input amps along the path so the mixer/selector
     * sums actually reach the DAC. Output amps are programmed by
     * hda_audio_apply_amps() below using the master volume.
     *
     * Amp Gain/Mute payload bits (verb 0x300):
     *   15: set output amp     14: set input amp
     *   13: set left            12: set right
     *   11:8 input index (0 here)
     *   7:   mute (0 = unmute)
     *   6:0: gain (0x7F = max)
     */
    const uint16_t in_left   = (1u << 14) | (1u << 13) | 0x7F;
    const uint16_t in_right  = (1u << 14) | (1u << 12) | 0x7F;

    for (uint8_t i = 0; i < path_len; i++) {
        uint16_t nid = path_nids[i];
        uint32_t wcap = codec_cmd(codec_addr, nid, VERB_GET_PARAM, PARAM_WIDGET_CAP);
        if (wcap & WCAP_IN_AMP) {
            codec_cmd(codec_addr, nid, VERB_SET_AMP_GAIN, in_left);
            codec_cmd(codec_addr, nid, VERB_SET_AMP_GAIN, in_right);
        }
    }

    /* Mark the codec path live and push the current master volume +
     * mute state to the DAC and pin output amps. */
    G_AUDIO_INITIALIZED = 1;
    hda_audio_apply_amps();

    uint32_t pcap = codec_cmd(codec_addr, pin_nid, VERB_GET_PARAM, PARAM_PIN_CAP);
    uint8_t pinctl = PIN_CTL_OUT_ENABLE;
    if (pcap & PIN_CAP_HEADPHONE) pinctl |= PIN_CTL_HP_ENABLE;
    codec_cmd(codec_addr, pin_nid, VERB_SET_PIN_CTL, pinctl);

    if (pcap & PIN_CAP_EAPD) {
        codec_cmd(codec_addr, pin_nid, VERB_SET_EAPD, 0x02);
    }

    return 0;
}

int hda_init(void)
{
    int n = pci_device_count();
    struct pci_device *dev = 0;
    for (int i = 0; i < n; i++) {
        struct pci_device *d = pci_get_device(i);
        if (d && d->class_code == 0x04 && d->subclass == 0x03) {
            dev = d; break;
        }
    }
    if (!dev) { status_msg = "no controller"; return -1; }

    uint32_t bar0 = dev->bar[0];
    if (bar0 & 1) { status_msg = "BAR0 is I/O"; return -1; }
    uint64_t bar = bar0 & 0xFFFFFFF0u;
    if ((bar0 & 0x06) == 0x04) {
        bar |= ((uint64_t)dev->bar[1]) << 32;
    }
    if (!bar) { status_msg = "BAR0 unmapped"; return -1; }
    mmio = (volatile uint8_t *)(uintptr_t)bar;

    uint32_t cmd = pci_config_read32(dev->bus, dev->dev, dev->func, 0x04);
    pci_config_write32(dev->bus, dev->dev, dev->func, 0x04, cmd | 0x06);

    kputs("hda: controller at ");
    kput_hex(bar);
    kputs(" (");
    kput_hex(dev->vendor_id);
    kputs(":");
    kput_hex(dev->device_id);
    kputs(")\n");

    if (hda_reset() < 0) { status_msg = "reset timeout"; return -1; }
    if (hda_setup_corb_rirb() < 0) return -1;

    timer_wait_ms(2);
    uint16_t sts = mr16(HDA_STATESTS) & 0x7FFF;
    if (!sts) { status_msg = "no codec"; return -1; }

    int found = 0;
    for (int cad = 0; cad < 15; cad++) {
        if (!(sts & (1 << cad))) continue;
        uint32_t vid = codec_cmd(cad, 0, VERB_GET_PARAM, PARAM_VENDOR_ID);
        if (vid == 0 || vid == 0xFFFFFFFFu) continue;
        kputs("hda: codec ");
        kput_dec(cad);
        kputs(" vendor=");
        kput_hex(vid);
        kputs("\n");
        if (hda_walk_codec(cad) == 0) {
            codec_addr = cad;
            found = 1;
            break;
        }
    }
    if (!found) { status_msg = "no usable output path"; return -1; }

    kputs("hda: codec ");
    kput_dec(codec_addr);
    kputs(" path:");
    for (uint8_t i = 0; i < path_len; i++) {
        uint16_t nid = path_nids[i];
        uint32_t wcap = codec_cmd(codec_addr, nid, VERB_GET_PARAM, PARAM_WIDGET_CAP);
        uint8_t  wt = (wcap >> 20) & 0xF;
        const char *tag = "?";
        switch (wt) {
            case WT_AUDIO_OUTPUT:   tag = "DAC"; break;
            case WT_AUDIO_MIXER:    tag = "MIX"; break;
            case WT_AUDIO_SELECTOR: tag = "SEL"; break;
            case WT_PIN_COMPLEX:    tag = "PIN"; break;
        }
        kputs(" ");
        kputs(tag);
        kputs(" NID ");
        kput_dec(nid);
        if (i + 1 < path_len) kputs(" ->");
    }
    kputs(" (output)\n");

    if (hda_setup_stream() < 0) return -1;

    /* Seed chain-node-private state from the bringup we just did. */
    s_pcm_source.pending.valid = 0;
    s_volume_filter.gain_q8    = 256;
    s_hda_pin.cad              = codec_addr;
    s_hda_pin.dac              = dac_nid;
    s_hda_pin.pin              = pin_nid;
    s_hda_pin.pushed           = 1;   /* hda_setup_stream already pushed once */
    s_hda_dma.sd_regs          = sd_regs;
    s_hda_dma.bdl_phys         = bdl_phys;

    ready = 1;
    status_msg = "ready";
    return 0;
}

int hda_ready(void) { return ready; }
const char *hda_status(void) { return status_msg; }

/* State accessors used by chain_registry to bind chain_node_t->state. */
void *hda_pcm_source_state(void)    { return &s_pcm_source; }
void *hda_volume_filter_state(void) { return &s_volume_filter; }
void *hda_pin_state(void)           { return &s_hda_pin; }
void *hda_dma_state(void)           { return &s_hda_dma; }

/* ── Chain node resolves ───────────────────────────────────────────── */
/*
 * Each resolve has the signature
 *   void f(chain_node_t *self, void *input, void *output)
 * and reads/writes scratch buffers supplied by chain_resolve.
 *
 * The four nodes form a linear pipeline; chain_resolve passes each
 * node's output to the next node's input.
 *
 *   pcm_source     : (module-state pending request) -> hda_pcm_frame_t
 *   volume_filter  : hda_pcm_frame_t                -> hda_pcm_frame_t (in-place scale)
 *   hda_pin        : hda_pcm_frame_t                -> hda_dma_descriptor_t
 *   hardware_dma   : hda_dma_descriptor_t           -> hda_tx_completion_t
 */

void hda_pcm_source_resolve(chain_node_t *self, void *input, void *output)
{
    (void)input;
    pcm_source_state_t *st = (pcm_source_state_t *)self->state;
    hda_pcm_frame_t *out = (hda_pcm_frame_t *)output;

    out->samples     = (int16_t *)0;
    out->num_samples = 0;
    out->sample_rate = 0;
    out->error       = 0;

    if (!st || !st->pending.valid) {
        out->error = 1;          /* nothing to play this tick -- benign idle */
        return;
    }

    /* Copy caller samples into the DMA-mapped audio_buf so downstream
     * nodes can mutate volume in place and hardware_dma can DMA from a
     * known physical address. Cap to buffer capacity. */
    int max_frames = HDA_AUDIO_BYTES / 4;          /* 16-bit stereo frames */
    int frames = st->pending.num_samples / 2;
    if (frames > max_frames) frames = max_frames;
    int n_int16 = frames * 2;

    int16_t *dst = (int16_t *)audio_buf;
    for (int i = 0; i < n_int16; i++) dst[i] = st->pending.samples[i];
    /* Zero remainder of buffer so the second BDL half doesn't replay stale audio. */
    for (int i = n_int16; i < HDA_AUDIO_BYTES / 2; i++) dst[i] = 0;

    out->samples     = dst;
    out->num_samples = n_int16;
    out->sample_rate = st->pending.sample_rate;
    out->error       = 0;

    /* Mark the request consumed. */
    st->pending.valid = 0;
}

void hda_volume_filter_resolve(chain_node_t *self, void *input, void *output)
{
    volume_filter_state_t *st = (volume_filter_state_t *)self->state;
    hda_pcm_frame_t *in  = (hda_pcm_frame_t *)input;
    hda_pcm_frame_t *out = (hda_pcm_frame_t *)output;

    /* Commit any pending volume/mute change first. This is how
     * hda_audio_set_volume / hda_audio_set_muted propagate: they stage
     * onto the node state and trigger a chain_resolve; the node owns
     * the apply + provenance bump. */
    if (st && st->pending_change) {
        int v = st->pending_volume;
        if (v < 0)   v = 0;
        if (v > 100) v = 100;
        st->volume = v;
        st->muted  = st->pending_muted ? 1 : 0;
        st->pending_change = 0;
        hda_audio_apply_amps();
        chain_t *cc = chain_get(CHAIN_AUDIO);
        if (cc) cc->vault_version++;
        (void)hda_audio_save_to_vault();
    }

    *out = *in;                         /* pass-through descriptor */
    if (in->error || !in->samples)
        return;

    /* Software backstop: if the codec path isn't initialized yet we
     * can't program hardware amps, so apply master volume / mute in
     * software here. Once the path is up the hardware amps own the
     * level and this filter stays at unity (gain_q8 = 256) to avoid
     * double-attenuation. The shell `volume`/`mute` commands keep
     * gain_q8 in sync via hda_audio_apply_amps(). */
    int gain;
    if (!G_AUDIO_INITIALIZED) {
        gain = G_AUDIO_MUTED ? 0 : ((G_AUDIO_VOLUME * 256) / 100);
    } else {
        gain = (st ? st->gain_q8 : 256);
    }
    if (gain == 256)
        return;                         /* unity -- no work */

    /* Scale samples in place: sample = sample * gain / 256.
     * Saturate at int16 bounds. */
    int n = in->num_samples;
    int16_t *s = in->samples;
    for (int i = 0; i < n; i++) {
        int32_t v = ((int32_t)s[i] * gain) >> 8;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        s[i] = (int16_t)v;
    }
}

void hda_pin_resolve(chain_node_t *self, void *input, void *output)
{
    hda_pin_state_t *st = (hda_pin_state_t *)self->state;
    hda_pcm_frame_t *in  = (hda_pcm_frame_t *)input;
    hda_dma_descriptor_t *out = (hda_dma_descriptor_t *)output;

    out->buf_phys    = audio_buf_phys;
    out->bytes       = HDA_AUDIO_BYTES;
    out->frames      = in->num_samples / 2;
    out->sample_rate = in->sample_rate;
    out->error       = in->error;

    if (in->error || !in->samples) return;

    /* Push pin/AMP settings if not yet applied. hda_init() already does
     * this once, so st->pushed starts at 1 -- but if a future caller
     * bumps gain or remaps the pin, we re-push here. */
    if (!st || st->pushed) return;

    /* Push the master volume / mute via verb 0x300 to DAC and pin. */
    hda_audio_apply_amps();

    uint32_t pcap = codec_cmd(st->cad, st->pin, VERB_GET_PARAM, PARAM_PIN_CAP);
    uint8_t pinctl = PIN_CTL_OUT_ENABLE;
    if (pcap & PIN_CAP_HEADPHONE) pinctl |= PIN_CTL_HP_ENABLE;
    codec_cmd(st->cad, st->pin, VERB_SET_PIN_CTL, pinctl);

    if (pcap & PIN_CAP_EAPD)
        codec_cmd(st->cad, st->pin, VERB_SET_EAPD, 0x02);

    st->pushed = 1;

    /* Pin/AMP state-changing operation: bump MasQ/VAULT version. */
    chain_t *c = chain_get(CHAIN_AUDIO);
    if (c) c->vault_version++;
}

void hda_dma_resolve(chain_node_t *self, void *input, void *output)
{
    hda_dma_state_t *st = (hda_dma_state_t *)self->state;
    hda_dma_descriptor_t *in  = (hda_dma_descriptor_t *)input;
    hda_tx_completion_t  *out = (hda_tx_completion_t *)output;

    out->ok            = 0;
    out->frames_played = 0;
    out->elapsed_ms    = 0;

    if (!st || !st->sd_regs || in->error || in->frames <= 0)
        return;

    volatile uint8_t *r = st->sd_regs;

    /* Stop any prior run. */
    sdw8_at(r, SD_CTL, sdr8_at(r, SD_CTL) & ~SDCTL_RUN);
    for (int i = 0; i < 100; i++) {
        if (!(sdr8_at(r, SD_CTL) & SDCTL_RUN)) break;
        busy_pause(1000);
    }

    /* Stream start = state change. */
    chain_t *c = chain_get(CHAIN_AUDIO);
    if (c) c->vault_version++;

    sdw8_at(r, SD_CTL, sdr8_at(r, SD_CTL) | SDCTL_RUN);

    uint32_t ms = (uint32_t)(((uint64_t)in->frames * 1000ULL) /
                             (uint64_t)(in->sample_rate ? in->sample_rate : 48000)) + 1;
    timer_wait_ms(ms);

    sdw8_at(r, SD_CTL, sdr8_at(r, SD_CTL) & ~SDCTL_RUN);
    if (c) c->vault_version++;

    out->ok            = 1;
    out->frames_played = in->frames;
    out->elapsed_ms    = ms;
}

/* ── Master volume + mute API ──────────────────────────────────────
 *
 * Pushes verb 0x300 (Set Amp Gain/Mute) to the DAC and pin output
 * amps. We program left and right separately, both for the output
 * amp side (bit 15). 0..100 maps linearly into the codec's 7-bit
 * gain register. Bit 7 is the per-payload mute flag.
 *
 * volume_filter's gain_q8 is held at 256 (unity) when the codec is
 * driving the level, so software volume doesn't double-attenuate. If
 * the codec ever stops being initialized (path lost), the filter
 * falls back to software scaling using g_audio directly.
 */

static uint16_t amp_payload(int set_left, int muted, int vol_pct)
{
    /* set output amp + L or R + mute + 7-bit gain */
    uint16_t pl = (1u << 15);
    pl |= set_left ? (1u << 13) : (1u << 12);
    if (muted) pl |= (1u << 7);
    /* 0..100 -> 0..0x7F with proper rounding. */
    int gain = ((vol_pct * 0x7F) + 50) / 100;
    if (gain < 0)    gain = 0;
    if (gain > 0x7F) gain = 0x7F;
    pl |= (uint16_t)(gain & 0x7F);
    return pl;
}

void hda_audio_apply_amps(void)
{
    /* Software filter stays at unity once hardware amps are in charge.
     * If the codec path isn't initialized yet, mirror the level into
     * gain_q8 so the volume_filter resolve applies it in software. */
    if (!G_AUDIO_INITIALIZED) {
        s_volume_filter.gain_q8 = G_AUDIO_MUTED
            ? 0
            : ((G_AUDIO_VOLUME * 256) / 100);
        return;
    }

    s_volume_filter.gain_q8 = 256;

    uint16_t left  = amp_payload(1, G_AUDIO_MUTED, G_AUDIO_VOLUME);
    uint16_t right = amp_payload(0, G_AUDIO_MUTED, G_AUDIO_VOLUME);

    /* Push to DAC's output amp if it has one, and to the pin's output
     * amp if it has one. We always attempt both — the codec ignores a
     * write to a non-existent amp (and the spec says the verb is safe
     * to issue regardless). */
    uint32_t dac_wcap = codec_cmd(codec_addr, dac_nid,
                                  VERB_GET_PARAM, PARAM_WIDGET_CAP);
    if (dac_wcap & WCAP_OUT_AMP) {
        codec_cmd(codec_addr, dac_nid, VERB_SET_AMP_GAIN, left);
        codec_cmd(codec_addr, dac_nid, VERB_SET_AMP_GAIN, right);
    }

    uint32_t pin_wcap = codec_cmd(codec_addr, pin_nid,
                                  VERB_GET_PARAM, PARAM_WIDGET_CAP);
    if (pin_wcap & WCAP_OUT_AMP) {
        codec_cmd(codec_addr, pin_nid, VERB_SET_AMP_GAIN, left);
        codec_cmd(codec_addr, pin_nid, VERB_SET_AMP_GAIN, right);
    }
}

int hda_audio_get_volume(void) { return G_AUDIO_VOLUME; }
int hda_audio_get_muted(void)  { return G_AUDIO_MUTED; }

/* Stage a volume / mute change onto CHAIN_AUDIO's volume_filter node
 * state and drive the change through chain_resolve. The volume_filter
 * resolve commits the staged values, calls apply_amps, bumps
 * vault_version, and persists to VAULT.
 *
 * Pre-CHAIN_AUDIO-registration callers (early hda_audio_restore_from_vault)
 * fall back to the direct mutation path so boot still works. After
 * registration every change is a chain pass. */
void hda_audio_set_volume(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    s_volume_filter.pending_volume = pct;
    s_volume_filter.pending_muted  = s_volume_filter.muted;
    s_volume_filter.pending_change = 1;

    if (CHAIN_AUDIO >= 0) {
        (void)chain_resolve(CHAIN_AUDIO);
        return;
    }
    /* Fallback (very early boot, pre-registration). */
    G_AUDIO_VOLUME = pct;
    hda_audio_apply_amps();
    (void)hda_audio_save_to_vault();
}

void hda_audio_set_muted(int muted)
{
    s_volume_filter.pending_volume = s_volume_filter.volume;
    s_volume_filter.pending_muted  = muted ? 1 : 0;
    s_volume_filter.pending_change = 1;

    if (CHAIN_AUDIO >= 0) {
        (void)chain_resolve(CHAIN_AUDIO);
        return;
    }
    G_AUDIO_MUTED = muted ? 1 : 0;
    hda_audio_apply_amps();
    (void)hda_audio_save_to_vault();
}

void hda_audio_toggle_mute(void)
{
    hda_audio_set_muted(!G_AUDIO_MUTED);
}

void hda_audio_volume_step(int delta)
{
    hda_audio_set_volume(G_AUDIO_VOLUME + delta);
}

/* ── Persistence ───────────────────────────────────────────────────
 *
 * /audio/volume in VAULT, INTERNAL tier. Written on every change so
 * a reboot picks up the last level the user set. Reads tolerate a
 * missing file (first boot) and discard records with a bad magic /
 * version. */

int hda_audio_save_to_vault(void)
{
    hda_audio_persist_t rec;
    rec.magic    = AUDIO_VAULT_MAGIC;
    rec.version  = (uint8_t)AUDIO_VAULT_VERSION;
    rec.volume   = (uint8_t)G_AUDIO_VOLUME;
    rec.muted    = (uint8_t)G_AUDIO_MUTED;
    rec.reserved = 0;

    /* mkdir is idempotent; ignore the return code. */
    (void)vault_mkdir("/audio", VAULT_TIER_INTERNAL);
    int n = vault_write(AUDIO_VAULT_PATH, &rec, sizeof(rec));
    return (n == (int)sizeof(rec)) ? 0 : -1;
}

int hda_audio_restore_from_vault(void)
{
    int sz = vault_size(AUDIO_VAULT_PATH);
    if (sz != (int)sizeof(hda_audio_persist_t)) return -1;

    hda_audio_persist_t rec;
    int n = vault_read(AUDIO_VAULT_PATH, &rec, sizeof(rec));
    if (n != (int)sizeof(rec)) return -1;
    if (rec.magic   != AUDIO_VAULT_MAGIC)   return -1;
    if (rec.version != AUDIO_VAULT_VERSION) return -1;

    int v = rec.volume;
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    G_AUDIO_VOLUME = v;
    G_AUDIO_MUTED  = rec.muted ? 1 : 0;
    hda_audio_apply_amps();
    return 0;
}

/* ── Compat shim ───────────────────────────────────────────────────── */

int hda_play_pcm(const int16_t *samples, int num_samples, int sample_rate)
{
    if (!ready) return -1;
    if (sample_rate != 48000) return -2;
    if (num_samples <= 0) return 0;
    if (CHAIN_AUDIO < 0) {
        /* Chain not yet registered (extremely early call) -- fall back to
         * the legacy direct path so the OS still makes sound. */
        sdw8(SD_CTL, sdr8(SD_CTL) & ~SDCTL_RUN);
        for (int i = 0; i < 100; i++) {
            if (!(sdr8(SD_CTL) & SDCTL_RUN)) break;
            busy_pause(1000);
        }
        int max_frames = HDA_AUDIO_BYTES / 4;
        int frames = num_samples / 2;
        if (frames > max_frames) frames = max_frames;
        int16_t *dst = (int16_t *)audio_buf;
        for (int i = 0; i < frames * 2; i++) dst[i] = samples[i];
        for (int i = frames * 2; i < HDA_AUDIO_BYTES / 2; i++) dst[i] = 0;
        sdw8(SD_CTL, sdr8(SD_CTL) | SDCTL_RUN);
        uint32_t ms = (uint32_t)(((uint64_t)frames * 1000ULL) / 48000ULL) + 1;
        timer_wait_ms(ms);
        sdw8(SD_CTL, sdr8(SD_CTL) & ~SDCTL_RUN);
        return 0;
    }

    /* Stage the request and resolve the chain. */
    s_pcm_source.pending.samples     = samples;
    s_pcm_source.pending.num_samples = num_samples;
    s_pcm_source.pending.sample_rate = sample_rate;
    s_pcm_source.pending.valid       = 1;
    s_pcm_source.pending.error       = 0;

    int rc = chain_resolve(CHAIN_AUDIO);
    return (rc == 0) ? 0 : -3;
}
