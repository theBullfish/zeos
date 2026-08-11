/*
 * Zeos -- USB HID (Boot Protocol) class driver
 *
 * Hooks USB boot-protocol mouse + keyboard onto the same input path
 * the PS/2 drivers feed. See usb_hid.h for the full design notes.
 *
 * Detection walks every device that xhci_init() enumerated, reads the
 * configuration descriptor, and looks for a HID interface (class 0x03,
 * sub 0x01 = boot, protocol 1 = keyboard or 2 = mouse) plus its single
 * interrupt-IN endpoint. Then it issues SET_CONFIGURATION /
 * SET_PROTOCOL(0) / SET_IDLE(0) and stands up the interrupt-IN ring.
 */

#include "usb_hid.h"
#include "usb.h"
#include "usb_xhci.h"
#include "keyboard.h"
#include "mouse.h"
#include "kprint.h"

extern void *memset(void *s, int c, unsigned long n);

/* ── HID descriptor types / requests ─────────────────────────── */
#define HID_DT_HID                  0x21
#define HID_DT_REPORT               0x22

#define HID_REQ_GET_REPORT          0x01
#define HID_REQ_GET_IDLE            0x02
#define HID_REQ_GET_PROTOCOL        0x03
#define HID_REQ_SET_REPORT          0x09
#define HID_REQ_SET_IDLE            0x0A
#define HID_REQ_SET_PROTOCOL        0x0B

#define HID_PROTO_BOOT              0x00
#define HID_PROTO_REPORT            0x01

/* HID class / boot subclass / protocols */
#define HID_IF_CLASS                0x03
#define HID_IF_BOOT_SUBCLASS        0x01
#define HID_IF_BOOT_KEYBOARD        0x01
#define HID_IF_BOOT_MOUSE           0x02

/* Endpoint descriptor attributes */
#define EP_TT_INTERRUPT             0x03

/* ── Per-attached-device state ───────────────────────────────── */
typedef enum {
    HID_KIND_NONE = 0,
    HID_KIND_KEYBOARD,
    HID_KIND_MOUSE,
} hid_kind_t;

#define USB_HID_MAX 4

typedef struct {
    struct xhci_device *dev;
    hid_kind_t kind;
    uint8_t interface_num;
    uint8_t ep_addr;
    uint16_t ep_max_packet;
    uint8_t  ep_interval;          /* xHCI Interval field */
    /* Last report (for keyboard rollover diff) */
    uint8_t last_report[8];
    /* Persistent buffer the xHCI ring writes into */
    uint8_t  rx_buf[16] __attribute__((aligned(64)));
} hid_dev_t;

static hid_dev_t g_hids[USB_HID_MAX];
static int g_hid_count;

/* ── Standard control-transfer helpers ───────────────────────── */
static int hid_set_protocol(struct xhci_device *dev, uint8_t iface, uint8_t proto)
{
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    sp.bRequest = HID_REQ_SET_PROTOCOL;
    sp.wValue = proto;
    sp.wIndex = iface;
    sp.wLength = 0;
    int r = xhci_control_transfer(dev, &sp, 0, 0);
    return (r < 0) ? -1 : 0;
}

static int hid_set_idle(struct xhci_device *dev, uint8_t iface,
                        uint8_t duration, uint8_t report_id)
{
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    sp.bRequest = HID_REQ_SET_IDLE;
    sp.wValue = ((uint16_t)duration << 8) | report_id;
    sp.wIndex = iface;
    sp.wLength = 0;
    int r = xhci_control_transfer(dev, &sp, 0, 0);
    return (r < 0) ? -1 : 0;
}

/* ── Compute xHCI Interval field from EP descriptor + speed ──── */
static uint8_t xhci_interval_from(int speed, uint8_t b_interval)
{
    /* xHCI spec table 6-12.
     *   FS / LS interrupt: bInterval is in frames (1..255).
     *     xHCI Interval = floor(log2(bInterval * 8))  (microframe units)
     *   HS interrupt: bInterval is in microframes (1..16).
     *     xHCI Interval = bInterval - 1
     *   SS interrupt: bInterval = 2^(Interval - 1) microframes,
     *     so xHCI Interval = bInterval - 1, range 0..15
     */
    if (b_interval == 0) b_interval = 1;
    if (speed == USB_SPEED_HIGH ||
        speed == USB_SPEED_SUPER ||
        speed == USB_SPEED_SUPER_PLUS) {
        if (b_interval > 16) b_interval = 16;
        return (uint8_t)(b_interval - 1);
    }
    /* FS / LS: log2(bInterval * 8) -> e.g. bInterval=10 -> 80 -> log2 ~= 6 */
    uint32_t v = (uint32_t)b_interval * 8;
    uint8_t k = 0;
    while ((v >> k) > 1 && k < 15) k++;
    return k;
}

/* ── HID Boot Keyboard usage -> set-1 scancode table ─────────── */
/* HID Keyboard/Keypad usage page (0x07). 0..3 = error codes. We map
 * the common usages (4..0x73) to set-1 scancodes. Unmapped entries
 * stay 0. Extended (E0-prefixed) keys would need extended=1 — for now
 * we don't emit extended scancodes (arrows etc.) since the existing
 * shell uses ASCII only. Brad's keybinds layer can be wired later. */
static const uint8_t hid_kb_to_sc1[256] = {
    [0x04] = 0x1E, /* a */ [0x05] = 0x30, /* b */ [0x06] = 0x2E, /* c */
    [0x07] = 0x20, /* d */ [0x08] = 0x12, /* e */ [0x09] = 0x21, /* f */
    [0x0A] = 0x22, /* g */ [0x0B] = 0x23, /* h */ [0x0C] = 0x17, /* i */
    [0x0D] = 0x24, /* j */ [0x0E] = 0x25, /* k */ [0x0F] = 0x26, /* l */
    [0x10] = 0x32, /* m */ [0x11] = 0x31, /* n */ [0x12] = 0x18, /* o */
    [0x13] = 0x19, /* p */ [0x14] = 0x10, /* q */ [0x15] = 0x13, /* r */
    [0x16] = 0x1F, /* s */ [0x17] = 0x14, /* t */ [0x18] = 0x16, /* u */
    [0x19] = 0x2F, /* v */ [0x1A] = 0x11, /* w */ [0x1B] = 0x2D, /* x */
    [0x1C] = 0x15, /* y */ [0x1D] = 0x2C, /* z */
    [0x1E] = 0x02, /* 1 */ [0x1F] = 0x03, /* 2 */ [0x20] = 0x04, /* 3 */
    [0x21] = 0x05, /* 4 */ [0x22] = 0x06, /* 5 */ [0x23] = 0x07, /* 6 */
    [0x24] = 0x08, /* 7 */ [0x25] = 0x09, /* 8 */ [0x26] = 0x0A, /* 9 */
    [0x27] = 0x0B, /* 0 */
    [0x28] = 0x1C, /* Enter   */
    [0x29] = 0x01, /* Esc     */
    [0x2A] = 0x0E, /* Backspc */
    [0x2B] = 0x0F, /* Tab     */
    [0x2C] = 0x39, /* Space   */
    [0x2D] = 0x0C, /* -       */
    [0x2E] = 0x0D, /* =       */
    [0x2F] = 0x1A, /* [       */
    [0x30] = 0x1B, /* ]       */
    [0x31] = 0x2B, /* \       */
    [0x33] = 0x27, /* ;       */
    [0x34] = 0x28, /* '       */
    [0x35] = 0x29, /* `       */
    [0x36] = 0x33, /* ,       */
    [0x37] = 0x34, /* .       */
    [0x38] = 0x35, /* /       */
    [0x39] = 0x3A, /* CapsLock */
};

/* HID modifier byte 0 -> set-1 left-shift / left-ctrl etc. We only
 * emit press/release for the modifiers the shell pipeline cares about;
 * keybinds_process handles modifier tracking. */
static void emit_modifier_diff(uint8_t prev, uint8_t curr)
{
    /* Bit 0 = LCtrl, 1 = LShift, 2 = LAlt, 3 = LGUI,
     * 4 = RCtrl, 5 = RShift, 6 = RAlt, 7 = RGUI. */
    static const uint8_t sc_press[8] = {
        0x1D, /* LCtrl */
        0x2A, /* LShift */
        0x38, /* LAlt */
        0x5B, /* LGUI (E0-prefixed in real HW; treat as plain) */
        0x1D, /* RCtrl (HW would prefix E0) */
        0x36, /* RShift */
        0x38, /* RAlt (HW would prefix E0) */
        0x5C, /* RGUI */
    };
    uint8_t diff = prev ^ curr;
    for (int b = 0; b < 8; b++) {
        if (!(diff & (1 << b))) continue;
        uint8_t sc = sc_press[b];
        if (curr & (1 << b))
            keyboard_inject_scancode(sc, 0);
        else
            keyboard_inject_scancode((uint8_t)(sc | 0x80), 0);
    }
}

/* Translate one boot keyboard report (8 bytes) into press/release events
 * relative to the previous report. Boot KB reports list up to 6
 * simultaneously-held usages in bytes 2..7. */
static void process_kb_report(hid_dev_t *h, const uint8_t *r)
{
    /* Modifier byte */
    emit_modifier_diff(h->last_report[0], r[0]);

    /* Key releases: anything in last_report not in current report */
    for (int i = 2; i < 8; i++) {
        uint8_t k = h->last_report[i];
        if (k < 4) continue;  /* 0..3 are error/no-event */
        int still_held = 0;
        for (int j = 2; j < 8; j++) {
            if (r[j] == k) { still_held = 1; break; }
        }
        if (!still_held) {
            uint8_t sc = hid_kb_to_sc1[k];
            if (sc) keyboard_inject_scancode((uint8_t)(sc | 0x80), 0);
        }
    }
    /* Key presses: anything in current not in previous */
    for (int i = 2; i < 8; i++) {
        uint8_t k = r[i];
        if (k < 4) continue;
        int already = 0;
        for (int j = 2; j < 8; j++) {
            if (h->last_report[j] == k) { already = 1; break; }
        }
        if (!already) {
            uint8_t sc = hid_kb_to_sc1[k];
            if (sc) keyboard_inject_scancode(sc, 0);
        }
    }
    for (int i = 0; i < 8; i++) h->last_report[i] = r[i];
}

static void process_mouse_report(hid_dev_t *h, const uint8_t *r, int len)
{
    (void)h;
    if (len < 3) return;
    uint8_t buttons = r[0] & 0x07;
    int dx = (int)(int8_t)r[1];
    int dy = (int)(int8_t)r[2];
    /* HID boot mouse Y is positive-down, matching screen coords -- no
     * inversion needed (unlike PS/2). */
    mouse_inject(dx, dy, buttons);
}

/* ── Configuration descriptor parsing ────────────────────────── */
/* Walk the descriptor blob looking for a HID boot interface and its
 * single interrupt-IN endpoint. Returns 1 on hit, 0 if no match.
 * Out params: *iface_num, *ep_addr, *ep_mp, *ep_bint, *kind. */
static int find_hid_in_config(const uint8_t *cfg, int total,
                              uint8_t *iface_num, uint8_t *ep_addr,
                              uint16_t *ep_mp, uint8_t *ep_bint,
                              hid_kind_t *kind)
{
    int off = 0;
    int in_hid_iface = 0;
    uint8_t cur_iface = 0;
    hid_kind_t cur_kind = HID_KIND_NONE;

    while (off + 2 <= total) {
        uint8_t blen = cfg[off];
        uint8_t btyp = cfg[off + 1];
        if (blen < 2 || off + blen > total) break;

        if (btyp == USB_DT_INTERFACE && blen >= 9) {
            uint8_t inum  = cfg[off + 2];
            uint8_t iclass = cfg[off + 5];
            uint8_t isub   = cfg[off + 6];
            uint8_t iproto = cfg[off + 7];
            if (iclass == HID_IF_CLASS && isub == HID_IF_BOOT_SUBCLASS) {
                if (iproto == HID_IF_BOOT_KEYBOARD) {
                    in_hid_iface = 1;
                    cur_iface = inum;
                    cur_kind = HID_KIND_KEYBOARD;
                } else if (iproto == HID_IF_BOOT_MOUSE) {
                    in_hid_iface = 1;
                    cur_iface = inum;
                    cur_kind = HID_KIND_MOUSE;
                } else {
                    in_hid_iface = 0;
                }
            } else {
                in_hid_iface = 0;
            }
        } else if (btyp == USB_DT_ENDPOINT && blen >= 7 && in_hid_iface) {
            uint8_t addr = cfg[off + 2];
            uint8_t attr = cfg[off + 3];
            uint16_t mp  = cfg[off + 4] | ((uint16_t)cfg[off + 5] << 8);
            uint8_t bint = cfg[off + 6];
            if ((addr & 0x80) && (attr & 0x03) == EP_TT_INTERRUPT) {
                *iface_num = cur_iface;
                *ep_addr   = addr;
                *ep_mp     = mp & 0x07FF;  /* low 11 bits */
                *ep_bint   = bint;
                *kind      = cur_kind;
                return 1;
            }
        }
        off += blen;
    }
    return 0;
}

/* ── Per-device attach ───────────────────────────────────────── */
static int hid_attach(struct xhci_device *dev)
{
    if (g_hid_count >= USB_HID_MAX) return -1;

    /* HID class lives at the interface level. The device-level class
     * is usually 0 (composite) or 0xEF (misc). We rely on the config
     * descriptor walk to find HID interfaces. */
    uint8_t cfgbuf[256];
    int got = xhci_get_config_descriptor(dev, cfgbuf, sizeof(cfgbuf));
    if (got < 9) return -1;

    uint8_t iface_num, ep_addr, ep_bint;
    uint16_t ep_mp;
    hid_kind_t kind;
    if (!find_hid_in_config(cfgbuf, got,
                            &iface_num, &ep_addr, &ep_mp, &ep_bint, &kind))
        return -1;

    /* SET_CONFIGURATION using bConfigurationValue from the descriptor.
     * Byte 5 of the config descriptor is bConfigurationValue. */
    uint8_t cfg_val = cfgbuf[5];
    if (xhci_set_configuration(dev, cfg_val) != 0) {
        kputs("[hid] SET_CONFIGURATION failed\n");
        return -1;
    }

    if (hid_set_protocol(dev, iface_num, HID_PROTO_BOOT) != 0) {
        /* Not fatal -- some QEMU revs reject SET_PROTOCOL on the boot
         * mouse. Boot devices default to boot protocol anyway. */
        kputs("[hid] SET_PROTOCOL(boot) rejected -- assuming default\n");
    }
    /* SET_IDLE(0) -- only report on state change. Keyboards typically
     * support this; some mice STALL it -- harmless. */
    (void)hid_set_idle(dev, iface_num, 0, 0);

    uint8_t xint = xhci_interval_from(dev->speed, ep_bint);
    if (xhci_setup_interrupt_in(dev, ep_addr, ep_mp, xint) != 0) {
        kputs("[hid] setup_interrupt_in failed\n");
        return -1;
    }

    hid_dev_t *h = &g_hids[g_hid_count++];
    memset(h, 0, sizeof(*h));
    h->dev = dev;
    h->kind = kind;
    h->interface_num = iface_num;
    h->ep_addr = ep_addr;
    h->ep_max_packet = ep_mp;
    h->ep_interval = xint;

    kputs("[hid] attached ");
    kputs(kind == HID_KIND_KEYBOARD ? "keyboard" : "mouse");
    kputs(" iface=");
    kput_dec(iface_num);
    kputs(" ep=");
    kput_hex(ep_addr);
    kputs(" mps=");
    kput_dec(ep_mp);
    kputs(" interval=");
    kput_dec(xint);
    kputs("\n");
    return 0;
}

int usb_hid_init(void)
{
    g_hid_count = 0;
    int n = xhci_device_count();
    int attached = 0;
    for (int i = 0; i < n; i++) {
        struct xhci_device *d = xhci_get_device(i);
        if (!d) continue;
        if (hid_attach(d) == 0) attached++;
    }
    if (attached) {
        kputs("[hid] ");
        kput_dec(attached);
        kputs(" device(s) ready\n");
    }
    return attached;
}

void usb_hid_poll(void)
{
    for (int i = 0; i < g_hid_count; i++) {
        hid_dev_t *h = &g_hids[i];
        int got = xhci_interrupt_poll(h->dev, h->rx_buf, sizeof(h->rx_buf));
        if (got <= 0) continue;
        if (h->kind == HID_KIND_KEYBOARD && got >= 8) {
            process_kb_report(h, h->rx_buf);
        } else if (h->kind == HID_KIND_MOUSE && got >= 3) {
            process_mouse_report(h, h->rx_buf, got);
        }
    }
}
