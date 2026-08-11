/*
 * Zeos -- Bluetooth USB transport
 *
 * See bt_usb.h for the layering rationale. This file is host-controller
 * glue only; nothing here understands HCI command opcodes.
 */

#include "bt_usb.h"
#include "usb.h"
#include "usb_xhci.h"
#include "kprint.h"

extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *s, int c, unsigned long n);

/* USB endpoint attribute masks. */
#define EP_TT_INTERRUPT     0x03
#define EP_TT_BULK          0x02

/* Standard HCI USB Transport endpoint addresses. Interface 0 is mandated
 * by the spec to expose:
 *   0x81 — interrupt IN  (HCI events)
 *   0x02 — bulk OUT      (HCI ACL data)
 *   0x82 — bulk IN       (HCI ACL data)
 * We confirm by walking the configuration descriptor — many real
 * dongles are USB composite (audio + HCI) so we cannot just hard-code. */

typedef struct {
    int                 in_use;
    struct xhci_device *dev;
    uint8_t             ep_event;       /* interrupt IN, e.g. 0x81 */
    uint16_t            ep_event_mp;
    uint8_t             ep_event_int;
    uint8_t             ep_acl_in;      /* bulk IN, e.g. 0x82 */
    uint16_t            ep_acl_in_mp;
    uint8_t             ep_acl_out;     /* bulk OUT, e.g. 0x02 */
    uint16_t            ep_acl_out_mp;
    uint8_t             cfg_value;
    uint8_t             iface_num;
    uint8_t             rx_buf[BT_HCI_PKT_MAX] __attribute__((aligned(64)));
    uint8_t             acl_rx[BT_ACL_PKT_MAX] __attribute__((aligned(64)));
} bt_xport_t;

static bt_xport_t g_bt;

/* xHCI Interval encoding identical to usb_hid.c::xhci_interval_from. */
static uint8_t bt_interval_from(int speed, uint8_t b_interval)
{
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

/* Walk a configuration descriptor blob looking for interface 0
 * (BT class) and pull out the three endpoints we need. Returns 1 if
 * all three were found, 0 otherwise. */
static int bt_walk_config(const uint8_t *cfg, int total,
                          uint8_t *cfg_value,
                          uint8_t *iface_num,
                          uint8_t *ep_evt, uint16_t *ep_evt_mp, uint8_t *ep_evt_int,
                          uint8_t *ep_in,  uint16_t *ep_in_mp,
                          uint8_t *ep_out, uint16_t *ep_out_mp)
{
    if (total < 9) return 0;
    *cfg_value = cfg[5];
    int off = 9;
    int in_bt_iface = 0;
    int got_evt = 0, got_in = 0, got_out = 0;
    uint8_t cur_iface = 0xFF;

    while (off + 2 <= total) {
        uint8_t blen = cfg[off];
        uint8_t btyp = cfg[off + 1];
        if (blen < 2 || off + blen > total) break;

        if (btyp == USB_DT_INTERFACE && blen >= 9) {
            uint8_t inum   = cfg[off + 2];
            uint8_t ialt   = cfg[off + 3];
            uint8_t iclass = cfg[off + 5];
            uint8_t isub   = cfg[off + 6];
            uint8_t iproto = cfg[off + 7];
            /* The HCI interface is interface 0 alt 0, class E0/01/01.
             * Some composite devices put HCI on interface 0 with the
             * class triple at the device level (and zero at the iface
             * level); accept either. */
            if (ialt == 0 &&
                ((iclass == BT_USB_CLASS && isub == BT_USB_SUBCLASS &&
                  iproto == BT_USB_PROTOCOL) ||
                 (iclass == 0x00 && isub == 0x00 && iproto == 0x00 && inum == 0))) {
                in_bt_iface = 1;
                cur_iface = inum;
            } else {
                in_bt_iface = 0;
            }
        } else if (btyp == USB_DT_ENDPOINT && blen >= 7 && in_bt_iface) {
            uint8_t addr = cfg[off + 2];
            uint8_t attr = cfg[off + 3] & 0x03;
            uint16_t mp  = (cfg[off + 4] | ((uint16_t)cfg[off + 5] << 8)) & 0x07FF;
            uint8_t bint = cfg[off + 6];
            int dir_in = (addr & 0x80) ? 1 : 0;
            if (attr == EP_TT_INTERRUPT && dir_in && !got_evt) {
                *ep_evt = addr;
                *ep_evt_mp = mp;
                *ep_evt_int = bint;
                *iface_num = cur_iface;
                got_evt = 1;
            } else if (attr == EP_TT_BULK && dir_in && !got_in) {
                *ep_in = addr;
                *ep_in_mp = mp;
                got_in = 1;
            } else if (attr == EP_TT_BULK && !dir_in && !got_out) {
                *ep_out = addr;
                *ep_out_mp = mp;
                got_out = 1;
            }
        }
        off += blen;
    }
    return (got_evt && got_in && got_out) ? 1 : 0;
}

/* Class-triple match either at device level or fall-through composite. */
static int bt_device_matches(struct xhci_device *xdev)
{
    if (!xdev || xdev->slot_id == 0) return 0;
    if (xdev->dev_class == BT_USB_CLASS &&
        xdev->dev_subclass == BT_USB_SUBCLASS &&
        xdev->dev_protocol == BT_USB_PROTOCOL) {
        return 1;
    }
    return 0;
}

static int bt_bind(struct xhci_device *xdev)
{
    uint8_t cfgbuf[256];
    int got = xhci_get_config_descriptor(xdev, cfgbuf, sizeof(cfgbuf));
    if (got < 9) return -1;

    uint8_t cfg_value = 1, iface_num = 0;
    uint8_t ep_evt = 0, ep_evt_int = 0, ep_in = 0, ep_out = 0;
    uint16_t ep_evt_mp = 0, ep_in_mp = 0, ep_out_mp = 0;

    if (!bt_walk_config(cfgbuf, got,
                        &cfg_value, &iface_num,
                        &ep_evt, &ep_evt_mp, &ep_evt_int,
                        &ep_in, &ep_in_mp,
                        &ep_out, &ep_out_mp)) {
        kputs("[bt-usb] descriptor walk: no full HCI endpoint set\n");
        return -1;
    }

    if (xhci_set_configuration(xdev, cfg_value) != 0) {
        kputs("[bt-usb] SET_CONFIGURATION failed\n");
        return -1;
    }

    uint8_t xint = bt_interval_from(xdev->speed, ep_evt_int ? ep_evt_int : 1);
    if (xhci_setup_interrupt_in(xdev, ep_evt, ep_evt_mp ? ep_evt_mp : 16, xint) != 0) {
        kputs("[bt-usb] event endpoint setup failed\n");
        return -1;
    }
    if (xhci_setup_bulk_endpoint(xdev, ep_in,  ep_in_mp  ? ep_in_mp  : 64) != 0) {
        kputs("[bt-usb] ACL IN setup failed\n");
        return -1;
    }
    if (xhci_setup_bulk_endpoint(xdev, ep_out, ep_out_mp ? ep_out_mp : 64) != 0) {
        kputs("[bt-usb] ACL OUT setup failed\n");
        return -1;
    }

    memset(&g_bt, 0, sizeof(g_bt));
    g_bt.in_use         = 1;
    g_bt.dev            = xdev;
    g_bt.cfg_value      = cfg_value;
    g_bt.iface_num      = iface_num;
    g_bt.ep_event       = ep_evt;
    g_bt.ep_event_mp    = ep_evt_mp;
    g_bt.ep_event_int   = xint;
    g_bt.ep_acl_in      = ep_in;
    g_bt.ep_acl_in_mp   = ep_in_mp;
    g_bt.ep_acl_out     = ep_out;
    g_bt.ep_acl_out_mp  = ep_out_mp;

    kputs("[bt-usb] bound vid=");
    kput_hex(xdev->vendor_id);
    kputs(" pid=");
    kput_hex(xdev->product_id);
    kputs(" iface=");
    kput_dec(iface_num);
    kputs(" ep_evt=");
    kput_hex(ep_evt);
    kputs(" ep_acl_in=");
    kput_hex(ep_in);
    kputs(" ep_acl_out=");
    kput_hex(ep_out);
    kputs("\n");
    return 0;
}

int bt_usb_init(void)
{
    g_bt.in_use = 0;
    int n = xhci_device_count();
    for (int i = 0; i < n; i++) {
        struct xhci_device *d = xhci_get_device(i);
        if (!bt_device_matches(d)) continue;
        if (bt_bind(d) == 0) return 0;
    }
    return -1;
}

int bt_usb_present(void) { return g_bt.in_use; }

int bt_usb_send_command(const void *pkt, int len)
{
    if (!g_bt.in_use) return -1;
    if (!pkt || len <= 0 || len > BT_HCI_PKT_MAX) return -1;

    /* The HCI command packet rides as the data stage of a class-specific
     * control transfer. Spec: bmRequestType=0x20, bRequest=0x00,
     * wValue=0, wIndex=0, wLength=len. */
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_DEVICE;
    sp.bRequest      = 0x00;
    sp.wValue        = 0;
    sp.wIndex        = 0;
    sp.wLength       = (uint16_t)len;

    /* xhci_control_transfer wants a writable buffer; copy into a local
     * aligned scratch so a const caller (e.g. literal command) is
     * acceptable AND the memory survives DMA. */
    static uint8_t cmd_bounce[BT_HCI_PKT_MAX] __attribute__((aligned(64)));
    memcpy(cmd_bounce, pkt, len);

    int r = xhci_control_transfer(g_bt.dev, &sp, cmd_bounce, len);
    return (r < 0) ? -1 : 0;
}

int bt_usb_poll_event(void *out, int max)
{
    if (!g_bt.in_use || !out || max <= 0) return -1;
    int got = xhci_interrupt_poll(g_bt.dev, g_bt.rx_buf, sizeof(g_bt.rx_buf));
    if (got <= 0) return got;
    int copy = (got < max) ? got : max;
    memcpy(out, g_bt.rx_buf, copy);
    return copy;
}

int bt_usb_acl_send(const void *pkt, int len)
{
    if (!g_bt.in_use) return -1;
    if (!pkt || len <= 0 || len > BT_ACL_PKT_MAX) return -1;
    /* Bounce so we present DMA-stable memory just like usb_cdc_send. */
    static uint8_t acl_bounce[BT_ACL_PKT_MAX] __attribute__((aligned(64)));
    memcpy(acl_bounce, pkt, len);
    int r = xhci_bulk_transfer(g_bt.dev, g_bt.ep_acl_out, acl_bounce, len, 0);
    return r;
}

int bt_usb_acl_poll(void *out, int max)
{
    if (!g_bt.in_use || !out || max <= 0) return -1;
    int got = xhci_bulk_poll_in(g_bt.dev, g_bt.ep_acl_in,
                                g_bt.acl_rx, sizeof(g_bt.acl_rx));
    if (got <= 0) return got;
    int copy = (got < max) ? got : max;
    memcpy(out, g_bt.acl_rx, copy);
    return copy;
}

void bt_usb_dump(void)
{
    if (!g_bt.in_use) {
        kputs("[bt-usb] no controller bound\n");
        return;
    }
    kputs("[bt-usb] vid=");
    kput_hex(g_bt.dev->vendor_id);
    kputs(" pid=");
    kput_hex(g_bt.dev->product_id);
    kputs(" cfg=");
    kput_dec(g_bt.cfg_value);
    kputs(" iface=");
    kput_dec(g_bt.iface_num);
    kputs(" ep_evt=");
    kput_hex(g_bt.ep_event);
    kputs(" ep_acl_in=");
    kput_hex(g_bt.ep_acl_in);
    kputs(" ep_acl_out=");
    kput_hex(g_bt.ep_acl_out);
    kputs("\n");
}
