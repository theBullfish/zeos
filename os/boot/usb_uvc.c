/*
 * USB UVC video class. xHCI iso transfers + Probe/Commit + UVC
 * payload header strip + JPEG reassembly. Default MJPEG 640x480@30.
 * CHAIN_VIDEO_IN emits typed video_frame signals; subscriber chains
 * (preview, future record/stream) consume.
 *
 * Camera-in-use indicator on panel — privacy-by-default.
 *
 * Limits:
 *   - QEMU has no UVC device by default. Real validation is on
 *     hardware (Logitech C270, generic chinese USB cams, etc).
 *   - Default profile is MJPEG 640x480 @ 30fps with the lowest
 *     bandwidth alt-setting that fits the chosen frame rate. We
 *     do not negotiate H.264 (rare on USB cams) or YUY2 fallback yet.
 *   - One iso-IN endpoint per camera. Multi-stream cameras (depth,
 *     IR, etc.) bind to the first stream only.
 */

#include "usb_uvc.h"
#include "usb.h"
#include "usb_xhci.h"
#include "chain.h"
#include "chain_registry.h"
#include "fat32.h"
#include "panel.h"
#include "kprint.h"

extern void *memset(void *s, int c, unsigned long n);
extern void *memcpy(void *dst, const void *src, unsigned long n);
extern uint64_t timer_read_tsc(void);
#define rdtsc() timer_read_tsc()

/* ── UVC class / subclass / descriptor codes ──────────────────── */
#define UVC_CC_VIDEO                    0x0E
#define UVC_SC_VIDEOCONTROL             0x01
#define UVC_SC_VIDEOSTREAMING           0x02
#define UVC_SC_VIDEO_INTERFACE_COLLECTION 0x03

#define UVC_CS_INTERFACE                0x24
#define UVC_CS_ENDPOINT                 0x25

/* VS interface descriptor subtypes */
#define UVC_VS_INPUT_HEADER             0x01
#define UVC_VS_FORMAT_UNCOMPRESSED      0x04
#define UVC_VS_FRAME_UNCOMPRESSED       0x05
#define UVC_VS_FORMAT_MJPEG             0x06
#define UVC_VS_FRAME_MJPEG              0x07

/* VC class-specific request codes (bRequest) */
#define UVC_SET_CUR                     0x01
#define UVC_GET_CUR                     0x81
#define UVC_GET_MIN                     0x82
#define UVC_GET_MAX                     0x83
#define UVC_GET_DEF                     0x87

/* VS control selector (wValue high byte) */
#define UVC_VS_PROBE_CONTROL            0x01
#define UVC_VS_COMMIT_CONTROL           0x02

/* Standard request: SET_INTERFACE */
#define USB_REQ_SET_INTERFACE           0x0B

/* UVC payload header bits */
#define UVC_PHDR_FID                    (1U << 0)
#define UVC_PHDR_EOF                    (1U << 1)
#define UVC_PHDR_ERR                    (1U << 6)

/* ── Per-camera state ────────────────────────────────────────── */
#define USB_UVC_MAX            4
#define UVC_FRAME_BUF_BYTES    (256 * 1024)   /* 256K — generous for 640x480 MJPEG */

typedef struct {
    int      configured;
    int      streaming;
    struct xhci_device *dev;

    uint8_t  vc_iface;          /* VideoControl interface */
    uint8_t  vs_iface;          /* VideoStreaming interface */
    uint8_t  ep_addr;           /* Iso IN endpoint */
    uint16_t ep_mps;            /* Max packet size at chosen alt setting */
    uint8_t  ep_alt;            /* Alt setting that exposes the iso EP */
    uint8_t  ep_pkts_per_frame; /* From wMaxPacketSize hi-bw bits */
    uint8_t  ep_interval;       /* xHCI Interval field */

    /* Negotiated format */
    int      format;            /* UVC_FORMAT_* */
    int      width;
    int      height;
    int      fps;
    uint8_t  format_idx;
    uint8_t  frame_idx;

    /* Reassembly buffer for the in-progress frame */
    uint8_t  asm_buf[UVC_FRAME_BUF_BYTES];
    int      asm_len;
    int      cur_fid;           /* 0/1 — last FID seen, -1 = none */

    /* One-shot completed JPEG ready for a consumer */
    uint8_t  done_buf[UVC_FRAME_BUF_BYTES];
    int      done_len;
    int      done_ready;
    uint64_t done_ts;

    /* Identity */
    uint16_t vid;
    uint16_t pid;

    /* Per-camera CHAIN_CAMERA_<n> id */
    int      chain_id;
} uvc_dev_t;

static uvc_dev_t g_uvc[USB_UVC_MAX];
static int g_uvc_count;

/* CHAIN_VIDEO_IN ids — owned by this module. */
int CHAIN_VIDEO_IN = -1;
int CHAIN_VIDEO_PREVIEW = -1;

/* ── Control helpers ─────────────────────────────────────────── */
static int uvc_class_get(struct xhci_device *dev, uint8_t req,
                         uint8_t cs, uint8_t iface, void *buf, int len)
{
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    sp.bRequest = req;
    sp.wValue = (uint16_t)cs << 8;
    sp.wIndex = iface;
    sp.wLength = (uint16_t)len;
    return xhci_control_transfer(dev, &sp, buf, len);
}

static int uvc_class_set(struct xhci_device *dev, uint8_t cs, uint8_t iface,
                         const void *buf, int len)
{
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    sp.bRequest = UVC_SET_CUR;
    sp.wValue = (uint16_t)cs << 8;
    sp.wIndex = iface;
    sp.wLength = (uint16_t)len;
    return xhci_control_transfer(dev, &sp, (void *)(uintptr_t)buf, len);
}

static int uvc_set_interface(struct xhci_device *dev, uint8_t iface,
                             uint8_t alt)
{
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE;
    sp.bRequest = USB_REQ_SET_INTERFACE;
    sp.wValue = alt;
    sp.wIndex = iface;
    sp.wLength = 0;
    return xhci_control_transfer(dev, &sp, 0, 0);
}

/* ── Descriptor parse ────────────────────────────────────────── */
typedef struct {
    /* Working state during the parse */
    int  vc_iface;
    int  vs_iface;
    int  vs_alt;            /* Alt setting that has the iso ep */
    int  iso_ep_addr;
    int  iso_ep_mps;
    int  iso_ep_pkts_per_frame;
    int  iso_ep_bint;

    /* Format/frame match */
    int  mjpeg_fmt_idx;     /* 1-based per UVC */
    int  mjpeg_frame_idx;   /* 1-based; we pick 640x480 */
    int  mjpeg_w;
    int  mjpeg_h;
    int  mjpeg_fps;         /* From dwFrameInterval = 10000000/fps */

    int  yuy2_fmt_idx;
    int  yuy2_frame_idx;
    int  yuy2_w;
    int  yuy2_h;
    int  yuy2_fps;
} uvc_parse_t;

static int parse_uvc_config(const uint8_t *cfg, int total, uvc_parse_t *p)
{
    memset(p, 0, sizeof(*p));
    p->vc_iface = -1;
    p->vs_iface = -1;
    p->vs_alt = -1;

    int off = 0;
    int cur_alt = 0;
    int in_vc = 0;
    int in_vs = 0;
    int cur_mjpeg_fmt = 0;     /* fmt index currently being walked, 0 if none */
    int cur_yuy2_fmt = 0;

    while (off + 2 <= total) {
        uint8_t blen = cfg[off];
        uint8_t btyp = cfg[off + 1];
        if (blen < 2 || off + blen > total) break;

        if (btyp == USB_DT_INTERFACE && blen >= 9) {
            uint8_t inum = cfg[off + 2];
            uint8_t alt  = cfg[off + 3];
            uint8_t icls = cfg[off + 5];
            uint8_t isub = cfg[off + 6];
            cur_alt = alt;
            in_vc = (icls == UVC_CC_VIDEO && isub == UVC_SC_VIDEOCONTROL);
            in_vs = (icls == UVC_CC_VIDEO && isub == UVC_SC_VIDEOSTREAMING);
            if (in_vc && p->vc_iface < 0) p->vc_iface = inum;
            if (in_vs && p->vs_iface < 0) p->vs_iface = inum;
            cur_mjpeg_fmt = 0;
            cur_yuy2_fmt = 0;
        } else if (btyp == UVC_CS_INTERFACE && in_vs && blen >= 3) {
            uint8_t sub = cfg[off + 2];
            if (sub == UVC_VS_FORMAT_MJPEG && blen >= 11) {
                p->mjpeg_fmt_idx = cfg[off + 3];
                cur_mjpeg_fmt = p->mjpeg_fmt_idx;
                cur_yuy2_fmt = 0;
            } else if (sub == UVC_VS_FORMAT_UNCOMPRESSED && blen >= 27) {
                /* GUID at offset 5 — first 4 bytes are FOURCC for YUY2:
                 * "YUY2" = 0x59 0x55 0x59 0x32. Match relaxed. */
                if (cfg[off + 5] == 0x59 && cfg[off + 6] == 0x55 &&
                    cfg[off + 7] == 0x59 && cfg[off + 8] == 0x32) {
                    p->yuy2_fmt_idx = cfg[off + 3];
                    cur_yuy2_fmt = p->yuy2_fmt_idx;
                    cur_mjpeg_fmt = 0;
                }
            } else if (sub == UVC_VS_FRAME_MJPEG && blen >= 26 && cur_mjpeg_fmt) {
                uint8_t fr_idx = cfg[off + 3];
                uint16_t w = cfg[off + 5] | ((uint16_t)cfg[off + 6] << 8);
                uint16_t h = cfg[off + 7] | ((uint16_t)cfg[off + 8] << 8);
                /* Frame interval is in 100ns units, little-endian, at
                 * offset 21 (default frame interval). */
                uint32_t fi = (uint32_t)cfg[off + 21] |
                              ((uint32_t)cfg[off + 22] << 8) |
                              ((uint32_t)cfg[off + 23] << 16) |
                              ((uint32_t)cfg[off + 24] << 24);
                int fps = (fi > 0) ? (int)(10000000U / fi) : 30;
                /* Prefer 640x480; otherwise track first hit. */
                if (p->mjpeg_w == 0 ||
                    (w == 640 && h == 480 && p->mjpeg_w != 640)) {
                    p->mjpeg_frame_idx = fr_idx;
                    p->mjpeg_w = w;
                    p->mjpeg_h = h;
                    p->mjpeg_fps = fps;
                }
            } else if (sub == UVC_VS_FRAME_UNCOMPRESSED && blen >= 26 && cur_yuy2_fmt) {
                uint8_t fr_idx = cfg[off + 3];
                uint16_t w = cfg[off + 5] | ((uint16_t)cfg[off + 6] << 8);
                uint16_t h = cfg[off + 7] | ((uint16_t)cfg[off + 8] << 8);
                uint32_t fi = (uint32_t)cfg[off + 21] |
                              ((uint32_t)cfg[off + 22] << 8) |
                              ((uint32_t)cfg[off + 23] << 16) |
                              ((uint32_t)cfg[off + 24] << 24);
                int fps = (fi > 0) ? (int)(10000000U / fi) : 30;
                if (p->yuy2_w == 0 ||
                    (w == 640 && h == 480 && p->yuy2_w != 640)) {
                    p->yuy2_frame_idx = fr_idx;
                    p->yuy2_w = w;
                    p->yuy2_h = h;
                    p->yuy2_fps = fps;
                }
            }
        } else if (btyp == USB_DT_ENDPOINT && blen >= 7 && in_vs) {
            uint8_t addr = cfg[off + 2];
            uint8_t attr = cfg[off + 3];
            uint16_t mp  = cfg[off + 4] | ((uint16_t)cfg[off + 5] << 8);
            uint8_t bint = cfg[off + 6];
            /* Iso IN endpoint inside a non-zero alt-setting. */
            if ((addr & 0x80) && (attr & 0x03) == 0x01 && cur_alt > 0) {
                int pkt = mp & 0x07FF;
                int hi = (mp >> 11) & 0x03;   /* additional transactions */
                int pkts = 1 + hi;
                /* Track the LARGEST mps alt setting that's still
                 * within our budget — gives best resolution headroom.
                 * If none picked yet, take this one. */
                int cap = pkt * pkts;
                int cur_cap = p->iso_ep_mps * (p->iso_ep_pkts_per_frame ?
                                               p->iso_ep_pkts_per_frame : 1);
                if (p->vs_alt < 0 || cap > cur_cap) {
                    p->vs_alt = cur_alt;
                    p->iso_ep_addr = addr;
                    p->iso_ep_mps = pkt;
                    p->iso_ep_pkts_per_frame = pkts;
                    p->iso_ep_bint = bint;
                }
            }
        }
        off += blen;
    }

    if (p->vc_iface < 0 || p->vs_iface < 0 || p->vs_alt < 0) return -1;
    if (p->mjpeg_fmt_idx == 0 && p->yuy2_fmt_idx == 0) return -1;
    return 0;
}

/* ── Probe / Commit ─────────────────────────────────────────── */
/* UVC 1.1 probe-control struct. We use a 26-byte version; cameras that
 * speak 1.5 will accept this and ignore tail fields. */
struct __attribute__((packed)) uvc_probe_v11 {
    uint16_t bmHint;
    uint8_t  bFormatIndex;
    uint8_t  bFrameIndex;
    uint32_t dwFrameInterval;
    uint16_t wKeyFrameRate;
    uint16_t wPFrameRate;
    uint16_t wCompQuality;
    uint16_t wCompWindowSize;
    uint16_t wDelay;
    uint32_t dwMaxVideoFrameSize;
    uint32_t dwMaxPayloadTransferSize;
};

static int uvc_negotiate(uvc_dev_t *u, uvc_parse_t *p)
{
    uint8_t fmt_idx, fr_idx;
    int format;
    int w, h, fps;

    if (p->mjpeg_fmt_idx) {
        fmt_idx = p->mjpeg_fmt_idx;
        fr_idx = p->mjpeg_frame_idx;
        format = UVC_FORMAT_MJPEG;
        w = p->mjpeg_w; h = p->mjpeg_h; fps = p->mjpeg_fps;
    } else {
        fmt_idx = p->yuy2_fmt_idx;
        fr_idx = p->yuy2_frame_idx;
        format = UVC_FORMAT_YUY2;
        w = p->yuy2_w; h = p->yuy2_h; fps = p->yuy2_fps;
    }
    if (fps <= 0) fps = 30;

    struct uvc_probe_v11 probe;
    memset(&probe, 0, sizeof(probe));
    probe.bmHint = 1;        /* dwFrameInterval is fixed */
    probe.bFormatIndex = fmt_idx;
    probe.bFrameIndex = fr_idx;
    probe.dwFrameInterval = (uint32_t)(10000000U / (uint32_t)fps);

    /* GET_DEF gives the camera's defaults — use it as a starting point
     * if available (UVC 1.1+); ignore failures. */
    struct uvc_probe_v11 def;
    if (uvc_class_get(u->dev, UVC_GET_DEF, UVC_VS_PROBE_CONTROL,
                      u->vs_iface, &def, sizeof(def)) == sizeof(def)) {
        probe.wDelay = def.wDelay;
    }

    /* SET_CUR (PROBE) */
    if (uvc_class_set(u->dev, UVC_VS_PROBE_CONTROL, u->vs_iface,
                      &probe, sizeof(probe)) < 0) {
        kputs("[uvc] probe SET_CUR failed\n");
        return -1;
    }
    /* GET_CUR — driver MUST honor what the camera echoes. */
    struct uvc_probe_v11 cur;
    int r = uvc_class_get(u->dev, UVC_GET_CUR, UVC_VS_PROBE_CONTROL,
                          u->vs_iface, &cur, sizeof(cur));
    if (r < (int)sizeof(cur)) {
        kputs("[uvc] probe GET_CUR short\n");
        return -1;
    }
    /* COMMIT — same payload the camera returned. */
    if (uvc_class_set(u->dev, UVC_VS_COMMIT_CONTROL, u->vs_iface,
                      &cur, sizeof(cur)) < 0) {
        kputs("[uvc] commit failed\n");
        return -1;
    }

    u->format = format;
    u->width = w;
    u->height = h;
    u->fps = fps;
    u->format_idx = cur.bFormatIndex;
    u->frame_idx = cur.bFrameIndex;
    return 0;
}

/* ── Iso callback: payload header strip + JPEG reassembly ────── */
static void uvc_iso_cb(void *user, const void *buf, int len)
{
    uvc_dev_t *u = (uvc_dev_t *)user;
    if (len < 2) return;
    const uint8_t *p = (const uint8_t *)buf;
    uint8_t hlen = p[0];
    uint8_t bfh  = p[1];
    if (hlen < 2 || hlen > len) return;
    if (bfh & UVC_PHDR_ERR) {
        /* Discard — bad payload, drop in-progress frame. */
        u->asm_len = 0;
        return;
    }
    int payload = len - hlen;
    int new_fid = (bfh & UVC_PHDR_FID) ? 1 : 0;

    /* FID toggle marks a new frame boundary. UVC: FID alternates per
     * frame; we accumulate into asm_buf and flush on EOF or on FID flip. */
    if (u->cur_fid >= 0 && new_fid != u->cur_fid) {
        /* Boundary without seeing EOF — flush partial as a frame
         * anyway (some cams skip EOF). */
        if (u->asm_len > 0 && !u->done_ready) {
            int n = u->asm_len;
            if (n > UVC_FRAME_BUF_BYTES) n = UVC_FRAME_BUF_BYTES;
            memcpy(u->done_buf, u->asm_buf, n);
            u->done_len = n;
            u->done_ts = rdtsc();
            u->done_ready = 1;
        }
        u->asm_len = 0;
    }
    u->cur_fid = new_fid;

    if (payload > 0) {
        int room = UVC_FRAME_BUF_BYTES - u->asm_len;
        int copy = (payload < room) ? payload : room;
        if (copy > 0) {
            memcpy(u->asm_buf + u->asm_len, p + hlen, copy);
            u->asm_len += copy;
        }
    }

    if (bfh & UVC_PHDR_EOF) {
        if (u->asm_len > 0 && !u->done_ready) {
            int n = u->asm_len;
            if (n > UVC_FRAME_BUF_BYTES) n = UVC_FRAME_BUF_BYTES;
            memcpy(u->done_buf, u->asm_buf, n);
            u->done_len = n;
            u->done_ts = rdtsc();
            u->done_ready = 1;
        }
        u->asm_len = 0;
    }
}

/* ── xHCI Interval helper (copied semantic from usb_hid.c) ───── */
static uint8_t xhci_interval_iso(int speed, uint8_t b_interval)
{
    /* For iso, FS bInterval is in frames (1..16) and HS/SS is
     * 2^(bInterval-1) microframes. xHCI Interval = bInterval - 1
     * for HS/SS; FS uses log2(bInterval*8). */
    if (b_interval == 0) b_interval = 1;
    if (speed == USB_SPEED_HIGH ||
        speed == USB_SPEED_SUPER ||
        speed == USB_SPEED_SUPER_PLUS) {
        if (b_interval > 16) b_interval = 16;
        return (uint8_t)(b_interval - 1);
    }
    uint32_t v = (uint32_t)b_interval * 8;
    uint8_t k = 0;
    while ((v >> k) > 1 && k < 15) k++;
    return k;
}

/* ── Per-device attach ───────────────────────────────────────── */
static int uvc_attach(struct xhci_device *dev)
{
    if (g_uvc_count >= USB_UVC_MAX) return -1;

    /* Class 0x0E may be at the device level (single-function cam) or
     * only at interface level (composite). Walk config either way. */
    uint8_t cfg[1024];
    int got = xhci_get_config_descriptor(dev, cfg, sizeof(cfg));
    if (got < 9) return -1;

    /* Quick scan: does this config contain ANY UVC interface? */
    int has_uvc = 0;
    for (int o = 0; o + 9 <= got; ) {
        uint8_t blen = cfg[o];
        uint8_t btyp = cfg[o + 1];
        if (blen < 2) break;
        if (btyp == USB_DT_INTERFACE) {
            if (cfg[o + 5] == UVC_CC_VIDEO &&
                (cfg[o + 6] == UVC_SC_VIDEOCONTROL ||
                 cfg[o + 6] == UVC_SC_VIDEOSTREAMING)) {
                has_uvc = 1; break;
            }
        }
        o += blen;
    }
    if (!has_uvc) return -1;

    uvc_parse_t pp;
    if (parse_uvc_config(cfg, got, &pp) != 0) {
        kputs("[uvc] descriptor parse failed\n");
        return -1;
    }

    uint8_t cfg_val = cfg[5];
    if (xhci_set_configuration(dev, cfg_val) != 0) {
        kputs("[uvc] SET_CONFIGURATION failed\n");
        return -1;
    }

    uvc_dev_t *u = &g_uvc[g_uvc_count];
    memset(u, 0, sizeof(*u));
    u->dev = dev;
    u->vc_iface = (uint8_t)pp.vc_iface;
    u->vs_iface = (uint8_t)pp.vs_iface;
    u->ep_addr = (uint8_t)pp.iso_ep_addr;
    u->ep_mps = (uint16_t)pp.iso_ep_mps;
    u->ep_alt = (uint8_t)pp.vs_alt;
    u->ep_pkts_per_frame = (uint8_t)pp.iso_ep_pkts_per_frame;
    u->ep_interval = xhci_interval_iso(dev->speed, (uint8_t)pp.iso_ep_bint);
    u->vid = dev->vendor_id;
    u->pid = dev->product_id;
    u->cur_fid = -1;
    u->configured = 1;
    u->chain_id = -1;

    if (uvc_negotiate(u, &pp) != 0) {
        return -1;
    }

    kputs("[uvc] attached vid=");
    kput_hex(u->vid);
    kputs(" pid=");
    kput_hex(u->pid);
    kputs(" fmt=");
    kputs(u->format == UVC_FORMAT_MJPEG ? "MJPEG" : "YUY2");
    kputs(" ");
    kput_dec(u->width);
    kputs("x");
    kput_dec(u->height);
    kputs("@");
    kput_dec(u->fps);
    kputs(" mps=");
    kput_dec(u->ep_mps);
    kputs(" alt=");
    kput_dec(u->ep_alt);
    kputs("\n");

    g_uvc_count++;
    return 0;
}

/* ── Public API ──────────────────────────────────────────────── */
int usb_uvc_init(void)
{
    g_uvc_count = 0;
    int n = xhci_device_count();
    int attached = 0;
    for (int i = 0; i < n; i++) {
        struct xhci_device *d = xhci_get_device(i);
        if (!d) continue;
        if (uvc_attach(d) == 0) attached++;
    }
    if (attached) {
        kputs("[uvc] ");
        kput_dec(attached);
        kputs(" UVC device(s) ready\n");
    }
    return attached;
}

int usb_uvc_count(void) { return g_uvc_count; }

int usb_uvc_describe(int idx, char *out, int max)
{
    if (idx < 0 || idx >= g_uvc_count || max < 32) return -1;
    uvc_dev_t *u = &g_uvc[idx];
    /* "vid:pid MJPEG 640x480@30" */
    int o = 0;
    static const char hex[] = "0123456789abcdef";
    for (int s = 12; s >= 0; s -= 4)
        if (o < max - 1) out[o++] = hex[(u->vid >> s) & 0xF];
    if (o < max - 1) out[o++] = ':';
    for (int s = 12; s >= 0; s -= 4)
        if (o < max - 1) out[o++] = hex[(u->pid >> s) & 0xF];
    if (o < max - 1) out[o++] = ' ';
    const char *f = (u->format == UVC_FORMAT_MJPEG) ? "MJPEG" : "YUY2";
    while (*f && o < max - 1) out[o++] = *f++;
    if (o < max - 1) out[o++] = ' ';
    /* width */
    char tmp[12]; int t;
    int v = u->width; t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = '0' + (v % 10); v /= 10; }
    while (t-- && o < max - 1) out[o++] = tmp[t];
    if (o < max - 1) out[o++] = 'x';
    v = u->height; t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = '0' + (v % 10); v /= 10; }
    while (t-- && o < max - 1) out[o++] = tmp[t];
    if (o < max - 1) out[o++] = '@';
    v = u->fps; t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = '0' + (v % 10); v /= 10; }
    while (t-- && o < max - 1) out[o++] = tmp[t];
    out[o] = 0;
    return o;
}

int usb_uvc_streaming(int idx)
{
    if (idx < 0 || idx >= g_uvc_count) return 0;
    return g_uvc[idx].streaming;
}

/* Forward — defined in panel.c */
extern void panel_indicator_camera(int active);

int usb_uvc_start(int idx)
{
    if (idx < 0 || idx >= g_uvc_count) return -1;
    uvc_dev_t *u = &g_uvc[idx];
    if (u->streaming) return 0;
    if (!u->configured) return -1;

    /* SET_INTERFACE(alt) on the streaming interface to bring up the
     * iso-IN endpoint exposed by that alt setting. */
    if (uvc_set_interface(u->dev, u->vs_iface, u->ep_alt) != 0) {
        kputs("[uvc] SET_INTERFACE(alt) failed\n");
        return -1;
    }

    if (xhci_iso_setup(u->dev, u->ep_addr, u->ep_mps,
                       u->ep_pkts_per_frame, u->ep_interval,
                       uvc_iso_cb, u) != 0) {
        kputs("[uvc] xhci_iso_setup failed\n");
        return -1;
    }
    if (xhci_iso_start(u->dev) != 0) {
        kputs("[uvc] xhci_iso_start failed\n");
        return -1;
    }
    u->streaming = 1;
    u->asm_len = 0;
    u->cur_fid = -1;
    u->done_ready = 0;

    /* MasQ: bump per-camera chain version. */
    if (u->chain_id >= 0) {
        chain_t *c = chain_get(u->chain_id);
        if (c) c->vault_version++;
    }
    panel_indicator_camera(1);
    return 0;
}

int usb_uvc_stop(int idx)
{
    if (idx < 0 || idx >= g_uvc_count) return -1;
    uvc_dev_t *u = &g_uvc[idx];
    if (!u->streaming) return 0;
    xhci_iso_stop(u->dev);
    /* Revert interface to alt 0 (zero-bandwidth). */
    (void)uvc_set_interface(u->dev, u->vs_iface, 0);
    u->streaming = 0;

    if (u->chain_id >= 0) {
        chain_t *c = chain_get(u->chain_id);
        if (c) c->vault_version++;
    }
    /* Clear panel dot if no other camera is still active. */
    int any = 0;
    for (int i = 0; i < g_uvc_count; i++)
        if (g_uvc[i].streaming) { any = 1; break; }
    if (!any) panel_indicator_camera(0);
    return 0;
}

int usb_uvc_dequeue_frame(int idx, video_frame_t *out)
{
    if (idx < 0 || idx >= g_uvc_count || !out) return -1;
    uvc_dev_t *u = &g_uvc[idx];
    if (!u->done_ready) return 0;
    out->camera_idx = idx;
    out->format = u->format;
    out->width = u->width;
    out->height = u->height;
    out->bytes_len = u->done_len;
    out->ts = u->done_ts;
    out->bytes = u->done_buf;
    u->done_ready = 0;
    return 1;
}

void usb_uvc_pump(void)
{
    for (int i = 0; i < g_uvc_count; i++) {
        if (g_uvc[i].streaming)
            xhci_iso_pump(g_uvc[i].dev);
    }
}

int usb_uvc_capture_jpeg(int idx, const char *path)
{
    if (idx < 0 || idx >= g_uvc_count || !path) return -1;
    uvc_dev_t *u = &g_uvc[idx];
    int started_here = 0;
    if (!u->streaming) {
        if (usb_uvc_start(idx) != 0) return -1;
        started_here = 1;
    }
    /* Pump until we get a frame or ~2s of pumps elapse (very loose). */
    video_frame_t vf;
    int got = 0;
    for (int n = 0; n < 4096 && !got; n++) {
        usb_uvc_pump();
        if (usb_uvc_dequeue_frame(idx, &vf) == 1) { got = 1; break; }
    }
    int rc = -1;
    if (got && vf.format == UVC_FORMAT_MJPEG && vf.bytes_len > 0) {
        if (fat32_create(path) >= 0) {
            int w = fat32_write(path, 0, vf.bytes, vf.bytes_len);
            if (w >= 0) rc = w;
        }
    }
    if (started_here) usb_uvc_stop(idx);
    return rc;
}

/* ── Chain pipeline ──────────────────────────────────────────── */
/* CHAIN_VIDEO_IN nodes:
 *   frame_request  → iso_poll       (no input, drives the pump)
 *   iso_poll       → reassemble     (xhci iso pump → payload bytes)
 *                                    Reassembly happens in the iso CB,
 *                                    so this node only verifies state.
 *   reassemble     → emit_frame     (handed to subscribers)
 * The chain emits a video_frame_t on each successful resolve where a
 * frame is ready; otherwise it emits a no-op completion. */

static void node_frame_request(chain_node_t *self, void *in, void *out) {
    (void)self; (void)in; (void)out;
    /* Source: trigger a pump cycle for every camera. */
    usb_uvc_pump();
}

static void node_iso_poll(chain_node_t *self, void *in, void *out) {
    (void)self; (void)in; (void)out;
    /* iso pump runs in node_frame_request; this node is a typed
     * hand-off point in the pipeline. State changes (start/stop) are
     * surfaced via per-camera chains, not here. */
}

static void node_reassemble(chain_node_t *self, void *in, void *out) {
    (void)self; (void)in; (void)out;
    /* Reassembly happens inside uvc_iso_cb on the iso-completion
     * boundary; this node just acknowledges. */
}

static video_frame_t g_last_emitted;

static void node_emit_frame(chain_node_t *self, void *in, void *out) {
    (void)self; (void)in;
    /* Try each camera in turn; emit the first ready frame. */
    for (int i = 0; i < g_uvc_count; i++) {
        if (!g_uvc[i].streaming) continue;
        if (usb_uvc_dequeue_frame(i, &g_last_emitted) == 1) {
            if (out) memcpy(out, &g_last_emitted, sizeof(g_last_emitted));
            return;
        }
    }
    /* No frame this tick — clear out so subscribers see len==0. */
    g_last_emitted.bytes_len = 0;
    if (out) memcpy(out, &g_last_emitted, sizeof(g_last_emitted));
}

/* CHAIN_VIDEO_PREVIEW: input video_frame, render to a small fb window. */
extern void fb_rect(int x, int y, int w, int h, uint32_t color);
extern void fb_text(int x, int y, const char *s, uint32_t color);
extern void fb_pixel(int x, int y, uint32_t color);

static void node_preview_render(chain_node_t *self, void *in, void *out) {
    (void)self; (void)out;
    if (!in) return;
    video_frame_t *vf = (video_frame_t *)in;
    if (vf->bytes_len == 0) return;
    /* Stub renderer: draw a small placeholder rectangle in the
     * top-left, with format + size text. JPEG decode is a separate
     * subsystem — when a software JPEG decoder lands, it plugs in
     * here. The stub demonstrates the chain wiring is real. */
    int W = 160, H = 120, X = 8, Y = 32;
    fb_rect(X, Y, W, H, 0xFF202020);
    /* Animate one row from the JPEG bytes as a "live" indicator. */
    int row = (int)(vf->ts >> 16) & (H - 4);
    for (int x = 0; x < W && x < vf->bytes_len; x++) {
        uint8_t b = vf->bytes[x];
        uint32_t c = 0xFF000000 | (b << 16) | (b << 8) | b;
        fb_pixel(X + x, Y + row, c);
    }
    fb_rect(X, Y, W, 1, 0xFFFF0000);  /* red top bar — privacy hint */
}

static int g_chain_camera_ids[USB_UVC_MAX];

void usb_uvc_register_chains(void)
{
    /* CHAIN_VIDEO_IN — root video chain on CPU. */
    CHAIN_VIDEO_IN = chain_create("video_in", CHAIN_CPU, MASQ_INTERNAL);
    if (CHAIN_VIDEO_IN >= 0) {
        chain_add_node(CHAIN_VIDEO_IN, "frame_request",
                       "tick", "iso_request", node_frame_request);
        chain_add_node(CHAIN_VIDEO_IN, "iso_poll",
                       "iso_request", "iso_payload", node_iso_poll);
        chain_add_node(CHAIN_VIDEO_IN, "reassemble",
                       "iso_payload", "video_frame", node_reassemble);
        chain_add_node(CHAIN_VIDEO_IN, "emit_frame",
                       "video_frame", "video_frame", node_emit_frame);
    }

    /* Per-camera CHAIN_CAMERA_<n> — currently a name-and-status chain
     * whose vault_version is bumped on every attach/start/stop. The
     * MasQ journal records every transition. */
    for (int i = 0; i < g_uvc_count; i++) {
        char nm[16] = "camera_";
        nm[7] = '0' + i;
        nm[8] = 0;
        int cid = chain_create(nm, CHAIN_VIDEO_IN, MASQ_INTERNAL);
        g_uvc[i].chain_id = cid;
        g_chain_camera_ids[i] = cid;
        if (cid >= 0) {
            chain_t *c = chain_get(cid);
            if (c) c->vault_version++;   /* attach event */
        }
    }

    /* CHAIN_VIDEO_PREVIEW — subscriber. Resolves at a slow cadence
     * (~10 Hz) to keep panel/desktop redraw cost low. */
    CHAIN_VIDEO_PREVIEW = chain_create("video_preview",
                                       CHAIN_VIDEO_IN, MASQ_INTERNAL);
    if (CHAIN_VIDEO_PREVIEW >= 0) {
        chain_add_node(CHAIN_VIDEO_PREVIEW, "render",
                       "video_frame", "tx_completion",
                       node_preview_render);
        chain_t *c = chain_get(CHAIN_VIDEO_PREVIEW);
        if (c) c->resolve_interval_ticks = 6;   /* ~10 Hz @ 60 tps */
    }
}

void usb_uvc_print_selftest_line(void)
{
    kputs("Cameras ............... ");
    if (g_uvc_count == 0) {
        kputs("not present\n");
        return;
    }
    kput_dec(g_uvc_count);
    kputs(" UVC device(s) — formats: ");
    for (int i = 0; i < g_uvc_count; i++) {
        if (i) kputs(", ");
        kputs(g_uvc[i].format == UVC_FORMAT_MJPEG ? "MJPEG" : "YUY2");
        kputs(" ");
        kput_dec(g_uvc[i].width);
        kputs("x");
        kput_dec(g_uvc[i].height);
        kputs("@");
        kput_dec(g_uvc[i].fps);
    }
    kputs("\n");
}
