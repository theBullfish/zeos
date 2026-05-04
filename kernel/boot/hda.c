/*
 * Zeos -- Intel HD Audio (HDA) driver
 *
 * Spec: Intel HD Audio Specification rev 1.0a (2010).
 *
 * Minimum-viable: PCI class 0x04 / subclass 0x03, MMIO at BAR0, CORB/RIRB
 * DMA rings for codec verbs, one BDL per output stream. Walks the codec
 * tree, finds an analog output pin, configures SD0 for 48 kHz/16-bit
 * stereo, plays PCM. Polling-only -- no MSI/IRQ. 48 kHz only.
 */

#include "hda.h"
#include "pci.h"
#include "pmm.h"
#include "kprint.h"
#include "timer.h"

#include <stdint.h>
#include <stddef.h>

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
#define VERB_SET_POWER             0x705
#define VERB_SET_PIN_CTL           0x707
#define VERB_SET_CONV_FMT          0x200
#define VERB_SET_CONV_STREAM_CHAN  0x706
#define VERB_SET_AMP_GAIN          0x300
#define VERB_SET_EAPD              0x70C

#define PARAM_VENDOR_ID            0x00
#define PARAM_NODE_COUNT           0x04
#define PARAM_FUNC_GROUP_TYPE      0x05
#define PARAM_WIDGET_CAP           0x09
#define PARAM_PIN_CAP              0x0C

#define WT_AUDIO_OUTPUT    0x0
#define WT_PIN_COMPLEX     0x4

#define PIN_CAP_OUTPUT      (1u << 4)
#define PIN_CAP_HEADPHONE   (1u << 3)
#define PIN_CAP_EAPD        (1u << 16)
#define PIN_CTL_OUT_ENABLE  (1u << 6)
#define PIN_CTL_HP_ENABLE   (1u << 7)

static volatile uint8_t  *mmio = 0;
static int                ready = 0;
static const char        *status_msg = "not initialized";

static uint32_t          *corb;
static uint64_t          *rirb;
static uint64_t           ring_phys;
static uint16_t           rirb_rp;

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

static int                out_sd_idx = -1;
static volatile uint8_t  *sd_regs = 0;

static inline uint8_t  mr8 (uint32_t off) { return *(volatile uint8_t  *)(mmio + off); }
static inline uint16_t mr16(uint32_t off) { return *(volatile uint16_t *)(mmio + off); }
static inline uint32_t mr32(uint32_t off) { return *(volatile uint32_t *)(mmio + off); }
static inline void     mw8 (uint32_t off, uint8_t  v) { *(volatile uint8_t  *)(mmio + off) = v; }
static inline void     mw16(uint32_t off, uint16_t v) { *(volatile uint16_t *)(mmio + off) = v; }
static inline void     mw32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(mmio + off) = v; }

static inline uint8_t  sdr8 (uint32_t off) { return *(volatile uint8_t  *)(sd_regs + off); }
static inline void     sdw8 (uint32_t off, uint8_t  v) { *(volatile uint8_t  *)(sd_regs + off) = v; }
static inline void     sdw16(uint32_t off, uint16_t v) { *(volatile uint16_t *)(sd_regs + off) = v; }
static inline void     sdw32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(sd_regs + off) = v; }

static void busy_pause(int loops)
{
    for (volatile int i = 0; i < loops; i++) __asm__ volatile("pause");
}

static uint32_t make_verb(uint8_t cad, uint16_t nid, uint16_t verb, uint16_t payload)
{
    uint32_t v = ((uint32_t)cad & 0xF) << 28;
    v |= ((uint32_t)nid & 0xFF) << 20;
    if ((verb & 0xF00) == 0x200 || (verb & 0xF00) == 0x300 ||
        (verb & 0xF00) == 0x700) {
        v |= ((uint32_t)(verb & 0xF00)) << 8;
        v |= (payload & 0xFFFF);
    } else {
        v |= ((uint32_t)(verb & 0xFFF)) << 8;
        v |= (payload & 0xFF);
    }
    return v;
}

static uint32_t codec_cmd(uint8_t cad, uint16_t nid, uint16_t verb, uint16_t payload)
{
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
            return (uint32_t)(r & 0xFFFFFFFFu);
        }
        busy_pause(100);
    }
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

static int hda_walk_codec(uint8_t cad)
{
    uint32_t r = codec_cmd(cad, 0, VERB_GET_PARAM, PARAM_NODE_COUNT);
    if (r == 0xFFFFFFFFu) return -1;
    uint16_t fg_start = (r >> 16) & 0xFF;
    uint16_t fg_count = r & 0xFF;

    for (uint16_t fg = fg_start; fg < fg_start + fg_count; fg++) {
        uint32_t ftype = codec_cmd(cad, fg, VERB_GET_PARAM, PARAM_FUNC_GROUP_TYPE);
        if ((ftype & 0xFF) != 0x01) continue;

        codec_cmd(cad, fg, VERB_SET_POWER, 0x00);

        uint32_t nc = codec_cmd(cad, fg, VERB_GET_PARAM, PARAM_NODE_COUNT);
        uint16_t w_start = (nc >> 16) & 0xFF;
        uint16_t w_count = nc & 0xFF;

        uint16_t found_dac = 0;
        uint16_t found_pin = 0;

        for (uint16_t w = w_start; w < w_start + w_count; w++) {
            uint32_t wcap = codec_cmd(cad, w, VERB_GET_PARAM, PARAM_WIDGET_CAP);
            uint8_t  wt = (wcap >> 20) & 0xF;

            if (wt == WT_AUDIO_OUTPUT && !found_dac) {
                found_dac = w;
            } else if (wt == WT_PIN_COMPLEX) {
                uint32_t pcap = codec_cmd(cad, w, VERB_GET_PARAM, PARAM_PIN_CAP);
                if (!(pcap & PIN_CAP_OUTPUT)) continue;
                uint32_t cfg = codec_cmd(cad, w, VERB_GET_CONFIG_DEFAULT, 0);
                uint8_t dev = (cfg >> 20) & 0xF;
                if (!found_pin && (dev == 0 || dev == 1 || dev == 2)) {
                    found_pin = w;
                }
            }
        }

        if (found_dac && found_pin) {
            dac_nid = found_dac;
            pin_nid = found_pin;
            return 0;
        }
        if (found_dac && !found_pin) {
            for (uint16_t w = w_start; w < w_start + w_count; w++) {
                uint32_t wcap = codec_cmd(cad, w, VERB_GET_PARAM, PARAM_WIDGET_CAP);
                if (((wcap >> 20) & 0xF) != WT_PIN_COMPLEX) continue;
                uint32_t pcap = codec_cmd(cad, w, VERB_GET_PARAM, PARAM_PIN_CAP);
                if (pcap & PIN_CAP_OUTPUT) {
                    dac_nid = found_dac;
                    pin_nid = w;
                    return 0;
                }
            }
        }
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

    codec_cmd(codec_addr, dac_nid, VERB_SET_POWER, 0x00);
    codec_cmd(codec_addr, pin_nid, VERB_SET_POWER, 0x00);

    codec_cmd(codec_addr, dac_nid, VERB_SET_AMP_GAIN, (1 << 15) | (1 << 13) | 0x7F);
    codec_cmd(codec_addr, dac_nid, VERB_SET_AMP_GAIN, (1 << 15) | (1 << 12) | 0x7F);
    codec_cmd(codec_addr, pin_nid, VERB_SET_AMP_GAIN, (1 << 15) | (1 << 13) | 0x7F);
    codec_cmd(codec_addr, pin_nid, VERB_SET_AMP_GAIN, (1 << 15) | (1 << 12) | 0x7F);

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

    kputs("hda: DAC nid=");
    kput_dec(dac_nid);
    kputs(" pin nid=");
    kput_dec(pin_nid);
    kputs("\n");

    if (hda_setup_stream() < 0) return -1;

    ready = 1;
    status_msg = "ready";
    return 0;
}

int hda_ready(void) { return ready; }
const char *hda_status(void) { return status_msg; }

int hda_play_pcm(const int16_t *samples, int num_samples, int sample_rate)
{
    if (!ready) return -1;
    if (sample_rate != 48000) return -2;
    if (num_samples <= 0) return 0;

    sdw8(SD_CTL, sdr8(SD_CTL) & ~SDCTL_RUN);
    for (int i = 0; i < 100; i++) {
        if (!(sdr8(SD_CTL) & SDCTL_RUN)) break;
        busy_pause(1000);
    }

    int max_frames = HDA_AUDIO_BYTES / 4;
    int frames = num_samples > max_frames ? max_frames : num_samples;
    int16_t *dst = (int16_t *)audio_buf;
    for (int i = 0; i < frames * 2; i++) dst[i] = samples[i];
    for (int i = frames * 2; i < HDA_AUDIO_BYTES / 2; i++) dst[i] = 0;

    sdw8(SD_CTL, sdr8(SD_CTL) | SDCTL_RUN);

    uint32_t ms = (uint32_t)(((uint64_t)frames * 1000ULL) / 48000ULL) + 1;
    timer_wait_ms(ms);

    sdw8(SD_CTL, sdr8(SD_CTL) & ~SDCTL_RUN);
    return 0;
}
