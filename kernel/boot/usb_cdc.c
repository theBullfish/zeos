/*
 * Zeos -- USB CDC ACM Class Driver
 *
 * Walks the configuration descriptor of every enumerated xHCI device,
 * detects a CDC-ACM data interface (or a known pseudo-CDC quirk), sets
 * the configuration, configures bulk IN + OUT endpoints, sends the
 * standard CDC class requests (SET_LINE_CODING + SET_CONTROL_LINE_STATE),
 * and exposes byte-stream I/O.
 *
 * Reference: USB CDC 1.2 + PSTN 1.2 (SET_LINE_CODING = class req 0x20,
 * SET_CONTROL_LINE_STATE = class req 0x22).
 */

#include "usb_cdc.h"
#include "usb_xhci.h"
#include "usb.h"
#include "kprint.h"

extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *s, int c, unsigned long n);

/* ── USB descriptor field offsets (parsing raw bytes) ────────── */
/* Common: bLength, bDescriptorType */
#define DESC_LEN(p)        ((p)[0])
#define DESC_TYPE(p)       ((p)[1])

/* Configuration descriptor */
#define DT_CONFIG          0x02
#define DT_INTERFACE       0x04
#define DT_ENDPOINT        0x05
#define DT_INTERFACE_ASSOC 0x0B
#define DT_CDC_CS_INTERFACE 0x24

/* CDC class codes */
#define USB_CLASS_CDC      0x02
#define USB_CLASS_CDC_DATA 0x0A
#define CDC_SUBCLASS_ACM   0x02

/* CDC class requests (PSTN) */
#define CDC_REQ_SET_LINE_CODING        0x20
#define CDC_REQ_SET_CONTROL_LINE_STATE 0x22

/* EP attribute bits */
#define EP_TYPE_BULK       0x02

/* Pseudo-CDC quirks (devices that don't strictly conform but are widely
 * used as USB serial). Matched on VID:PID. */
struct cdc_quirk {
    uint16_t vid;
    uint16_t pid;
    const char *name;
};

static const struct cdc_quirk quirk_table[] = {
    /* CP210x (UART bridges, ESP32 dev kits) — vendor specific */
    {0x10C4, 0xEA60, "cp2102"},
    {0x10C4, 0xEA70, "cp2105"},
    /* CH340 / CH341 — vendor specific */
    {0x1A86, 0x7523, "ch340"},
    {0x1A86, 0x5523, "ch341"},
    /* FTDI FT232 (vendor) — most Arduino clones */
    {0x0403, 0x6001, "ft232"},
    {0x0403, 0x6010, "ft2232"},
    {0x0403, 0x6011, "ft4232"},
    {0x0403, 0x6014, "ft232h"},
    {0x0403, 0x6015, "ft231x"},
    /* Pi Pico stdio (Raspberry Pi RP2040 USB CDC) — actually CDC-conformant
     * but listed for visibility. Class match handles it. */
    {0x2E8A, 0x000A, "pico-stdio"},
    /* ESP32-S2/S3 native USB CDC — CDC-conformant */
    {0x303A, 0x1001, "esp32s3"},
    {0x303A, 0x4001, "esp32c3"},
    /* Arduino Uno R3 (16U2 ACM) */
    {0x2341, 0x0043, "arduino-uno"},
    /* QEMU usb-serial */
    {0x0403, 0xFA00, "qemu-usb-serial"},  /* sometimes reuses FTDI VID */
    /* Sentinel */
    {0, 0, 0},
};

static const char *quirk_match(uint16_t vid, uint16_t pid)
{
    for (int i = 0; quirk_table[i].vid; i++) {
        if (quirk_table[i].vid == vid && quirk_table[i].pid == pid)
            return quirk_table[i].name;
    }
    return 0;
}

/* ── Driver state ────────────────────────────────────────────── */
static struct usb_cdc_device cdc_devs[USB_CDC_MAX_DEVICES];
static int cdc_count = 0;

/* ── Helpers: integer to dec ASCII for naming ────────────────── */
static void name_set(char *out, const char *prefix, int idx)
{
    int i = 0;
    while (prefix[i] && i < 12) { out[i] = prefix[i]; i++; }
    /* simple int -> ascii */
    if (idx == 0) {
        out[i++] = '0';
    } else {
        char buf[6];
        int n = 0, v = idx;
        while (v > 0 && n < 6) { buf[n++] = '0' + (v % 10); v /= 10; }
        while (n > 0 && i < 15) out[i++] = buf[--n];
    }
    out[i] = 0;
}

/* ── CDC class-request helpers ───────────────────────────────── */

struct cdc_line_coding {
    uint32_t dwDTERate;     /* baud */
    uint8_t  bCharFormat;   /* stop bits: 0=1, 1=1.5, 2=2 */
    uint8_t  bParityType;   /* 0=none */
    uint8_t  bDataBits;     /* 5,6,7,8,16 */
} __attribute__((packed));

static int cdc_set_line_coding(struct xhci_device *xdev, int iface,
                               uint32_t baud, uint8_t stop, uint8_t parity,
                               uint8_t data_bits)
{
    struct cdc_line_coding lc;
    lc.dwDTERate = baud;
    lc.bCharFormat = stop;
    lc.bParityType = parity;
    lc.bDataBits = data_bits;

    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    sp.bRequest = CDC_REQ_SET_LINE_CODING;
    sp.wValue = 0;
    sp.wIndex = (uint16_t)iface;
    sp.wLength = sizeof(lc);
    return xhci_control_transfer(xdev, &sp, &lc, sizeof(lc));
}

static int cdc_set_control_line_state(struct xhci_device *xdev, int iface,
                                      int dtr, int rts)
{
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    sp.bRequest = CDC_REQ_SET_CONTROL_LINE_STATE;
    sp.wValue = (dtr ? 1 : 0) | (rts ? 2 : 0);
    sp.wIndex = (uint16_t)iface;
    sp.wLength = 0;
    return xhci_control_transfer(xdev, &sp, 0, 0);
}

/* ── Config descriptor walker ────────────────────────────────── */

/* For a single xhci_device, parse its config descriptor and look for a
 * CDC ACM data interface (or, for a quirk-matched pseudo-CDC device, the
 * first interface that has bulk IN + OUT). On match, fills out_iface,
 * out_ep_in, out_ep_out, out_mps_in, out_mps_out with the data-interface
 * number and the two bulk endpoint addresses + max packet sizes.
 *
 * Returns 0 on success, -1 on no CDC interface, -2 on parse error,
 * -3 on quirk-match without a bulk pair (unsupported vendor protocol). */
static int cdc_walk_descriptor(const uint8_t *cfg, int total,
                               int allow_quirk_any_iface,
                               int *out_iface,
                               uint8_t *out_ep_in, uint8_t *out_ep_out,
                               uint16_t *out_mps_in, uint16_t *out_mps_out,
                               uint8_t *out_config_value)
{
    if (total < 9 || cfg[1] != DT_CONFIG) return -2;
    *out_config_value = cfg[5];  /* bConfigurationValue */

    /* Single pass: find a candidate interface that has bulk IN and OUT. */
    int p = cfg[0];  /* skip cfg header */

    int cur_iface = -1;
    int cur_class = -1;
    int cur_sub = -1;
    int cur_has_in = 0;
    int cur_has_out = 0;
    uint8_t cur_in_addr = 0, cur_out_addr = 0;
    uint16_t cur_in_mps = 0, cur_out_mps = 0;

    /* Simple state machine: when we hit a new INTERFACE descriptor, if
     * the previous interface satisfied our criteria we return. Otherwise
     * reset and continue. */
    while (p + 2 <= total) {
        int dlen = cfg[p];
        int dtype = cfg[p + 1];
        if (dlen < 2 || p + dlen > total) break;

        if (dtype == DT_INTERFACE) {
            /* Commit-or-discard previous interface. */
            if (cur_iface >= 0 && cur_has_in && cur_has_out) {
                int match = 0;
                if (cur_class == USB_CLASS_CDC_DATA) match = 1;
                else if (allow_quirk_any_iface) match = 1;
                if (match) {
                    *out_iface = cur_iface;
                    *out_ep_in = cur_in_addr;
                    *out_ep_out = cur_out_addr;
                    *out_mps_in = cur_in_mps;
                    *out_mps_out = cur_out_mps;
                    return 0;
                }
            }
            if (dlen >= 9) {
                cur_iface  = cfg[p + 2];   /* bInterfaceNumber */
                cur_class  = cfg[p + 5];   /* bInterfaceClass */
                cur_sub    = cfg[p + 6];   /* bInterfaceSubClass */
                (void)cur_sub;
            } else {
                cur_iface = -1;
            }
            cur_has_in = 0; cur_has_out = 0;
            cur_in_addr = cur_out_addr = 0;
            cur_in_mps = cur_out_mps = 0;
        } else if (dtype == DT_ENDPOINT) {
            if (dlen >= 7 && cur_iface >= 0) {
                uint8_t ep_addr = cfg[p + 2];
                uint8_t attrs   = cfg[p + 3];
                uint16_t mps    = (uint16_t)cfg[p + 4] | ((uint16_t)cfg[p + 5] << 8);
                if ((attrs & 0x3) == EP_TYPE_BULK) {
                    if (ep_addr & 0x80) {
                        cur_has_in = 1;
                        cur_in_addr = ep_addr;
                        cur_in_mps = mps & 0x07FF;
                    } else {
                        cur_has_out = 1;
                        cur_out_addr = ep_addr;
                        cur_out_mps = mps & 0x07FF;
                    }
                }
            }
        }
        p += dlen;
    }

    /* Final commit */
    if (cur_iface >= 0 && cur_has_in && cur_has_out) {
        int match = 0;
        if (cur_class == USB_CLASS_CDC_DATA) match = 1;
        else if (allow_quirk_any_iface) match = 1;
        if (match) {
            *out_iface = cur_iface;
            *out_ep_in = cur_in_addr;
            *out_ep_out = cur_out_addr;
            *out_mps_in = cur_in_mps;
            *out_mps_out = cur_out_mps;
            return 0;
        }
    }
    return -1;
}

/* ── Bind a single xHCI device as CDC-ACM ────────────────────── */

static int cdc_bind(struct xhci_device *xdev)
{
    if (cdc_count >= USB_CDC_MAX_DEVICES) return -1;
    struct usb_cdc_device *cd = &cdc_devs[cdc_count];

    /* Reject USB-Ethernet flavors of CDC — those belong to usb_eth.
     * Check device descriptor first (some devices set subclass at the
     * device level), then walk interface descriptors.
     *   0x06 = ECM, 0x0D = NCM, 0x0E = MBIM (also networky) */
    if (xdev->dev_class == USB_CLASS_CDC &&
        (xdev->dev_subclass == 0x06 || xdev->dev_subclass == 0x0D ||
         xdev->dev_subclass == 0x0E)) {
        return -1;
    }

    /* Read config descriptor (full hierarchy). */
    uint8_t cfg[256];
    int got = xhci_get_config_descriptor(xdev, cfg, sizeof(cfg));
    if (got >= 9) {
        int wp = cfg[0];
        while (wp + 2 <= got) {
            int dl = cfg[wp];
            int dt = cfg[wp + 1];
            if (dl < 2 || wp + dl > got) break;
            if (dt == DT_INTERFACE && dl >= 9) {
                uint8_t icl = cfg[wp + 5];
                uint8_t isc = cfg[wp + 6];
                /* Defer USB-Ethernet (ECM/NCM/MBIM) to usb_eth. */
                if (icl == USB_CLASS_CDC &&
                    (isc == 0x06 || isc == 0x0D || isc == 0x0E))
                    return -1;
            }
            wp += dl;
        }
    }
    if (got < 9) {
        kputs("[usb-cdc] config descriptor read failed for ");
        kput_hex(xdev->vendor_id); kputs(":");
        kput_hex(xdev->product_id); kputs("\n");
        return -1;
    }

    /* Decide whether to allow the quirk path (any-iface bulk pair).
     * Strict CDC: device class 02 OR an interface advertising 0x0A.
     * Quirk: VID/PID in our table. */
    const char *qname = quirk_match(xdev->vendor_id, xdev->product_id);
    int strict_cdc = (xdev->dev_class == USB_CLASS_CDC) ||
                     (xdev->dev_class == 0xEF /* IAD */) ||
                     (xdev->dev_class == 0x00);
    int allow_any = qname ? 1 : 0;

    int iface = -1;
    uint8_t ep_in = 0, ep_out = 0;
    uint16_t mps_in = 0, mps_out = 0;
    uint8_t cfg_value = 1;

    int r = cdc_walk_descriptor(cfg, got, allow_any,
                                &iface, &ep_in, &ep_out,
                                &mps_in, &mps_out, &cfg_value);
    if (r != 0) {
        /* If first pass with strict matching failed but the device is in
         * our quirk list, the walker already searched any-iface. Nothing
         * else to try. */
        (void)strict_cdc;
        return -1;
    }

    /* SET_CONFIGURATION */
    if (xhci_set_configuration(xdev, cfg_value) != 0) {
        kputs("[usb-cdc] SET_CONFIGURATION failed\n");
        return -1;
    }

    /* Configure both endpoints on the host controller. */
    if (xhci_setup_bulk_endpoint(xdev, ep_out, mps_out) != 0) {
        kputs("[usb-cdc] bulk OUT setup failed\n");
        return -1;
    }
    if (xhci_setup_bulk_endpoint(xdev, ep_in, mps_in) != 0) {
        kputs("[usb-cdc] bulk IN setup failed\n");
        return -1;
    }

    /* Issue CDC class requests if this is a strict CDC device. For the
     * vendor quirks (FTDI/CP210x/CH340) the protocol is different and we
     * skip — host can still tx/rx raw bytes via bulk, which is what most
     * loopback cases need. */
    if (xdev->dev_class == USB_CLASS_CDC || xdev->dev_class == 0xEF ||
        xdev->dev_class == 0x00) {
        /* 115200 8N1 */
        cdc_set_line_coding(xdev, iface, 115200, 0, 0, 8);
        cdc_set_control_line_state(xdev, iface, 1, 1);
    }

    /* Bind */
    memset(cd, 0, sizeof(*cd));
    cd->in_use = 1;
    cd->xdev = xdev;
    cd->iface_data = iface;
    cd->ep_in = ep_in;
    cd->ep_out = ep_out;
    cd->max_packet_in = mps_in ? mps_in : 64;
    cd->max_packet_out = mps_out ? mps_out : 64;
    cd->rx_head = cd->rx_tail = 0;
    cd->rx_armed = 0;
    name_set(cd->name, "ttyUSB", cdc_count);

    kputs("[usb-cdc] detected /dev/");
    kputs(cd->name);
    kputs(" vid=");
    kput_hex(xdev->vendor_id);
    kputs(" pid=");
    kput_hex(xdev->product_id);
    if (qname) { kputs(" ("); kputs(qname); kputs(")"); }
    kputs(" iface=");
    kput_dec(iface);
    kputs(" ep_in=");
    kput_hex(ep_in);
    kputs(" ep_out=");
    kput_hex(ep_out);
    kputs(" mps=");
    kput_dec(mps_in);
    kputs("\n");

    cdc_count++;
    return 0;
}

/* ── Public init ─────────────────────────────────────────────── */

int usb_cdc_init(void)
{
    cdc_count = 0;
    int n = xhci_device_count();
    for (int i = 0; i < n; i++) {
        struct xhci_device *d = xhci_get_device(i);
        if (!d || d->slot_id == 0) continue;
        cdc_bind(d);
    }
    if (cdc_count == 0) {
        kputs("[usb-cdc] no USB serial devices detected\n");
    } else {
        kputs("[usb-cdc] ready, ");
        kput_dec(cdc_count);
        kputs(" device(s) bound\n");
    }
    return cdc_count;
}

int usb_cdc_count(void) { return cdc_count; }

struct usb_cdc_device *usb_cdc_get(int idx)
{
    if (idx < 0 || idx >= cdc_count) return 0;
    if (!cdc_devs[idx].in_use) return 0;
    return &cdc_devs[idx];
}

/* ── Send (blocking, chunked by max_packet) ──────────────────── */

int usb_cdc_send(struct usb_cdc_device *dev, const void *buf, int len)
{
    if (!dev || !dev->in_use || len < 0) return -1;
    if (len > 0 && !buf) return -1;
    if (len == 0) return 0;

    /* Bulk OUT. We use a small bounce buffer because xhci_bulk_transfer
     * needs a persistent physical address — caller's buffer might live
     * in stack memory we'd rather not expose to DMA. */
    static uint8_t bounce[512] __attribute__((aligned(64)));
    const uint8_t *src = (const uint8_t *)buf;
    int sent = 0;
    while (sent < len) {
        int chunk = len - sent;
        if (chunk > (int)sizeof(bounce)) chunk = sizeof(bounce);
        memcpy(bounce, src + sent, chunk);
        int r = xhci_bulk_transfer(dev->xdev, dev->ep_out, bounce, chunk, 0);
        if (r < 0) return sent ? sent : -1;
        sent += r;
        if (r < chunk) break;
    }
    return sent;
}

/* ── Pump: drain bulk-IN into the rx ring ────────────────────── */

static int rx_ring_free(const struct usb_cdc_device *dev)
{
    /* Free space = SIZE - 1 - (tail - head) modulo SIZE. We reserve one
     * slot to disambiguate full/empty. */
    uint32_t used = (dev->rx_tail - dev->rx_head) & (USB_CDC_RX_RING_SIZE - 1);
    return (int)(USB_CDC_RX_RING_SIZE - 1 - used);
}

static void rx_ring_push(struct usb_cdc_device *dev, const uint8_t *data, int n)
{
    int free_n = rx_ring_free(dev);
    if (n > free_n) n = free_n;
    for (int i = 0; i < n; i++) {
        dev->rx_buf[dev->rx_tail] = data[i];
        dev->rx_tail = (dev->rx_tail + 1) & (USB_CDC_RX_RING_SIZE - 1);
    }
}

static void cdc_pump_one(struct usb_cdc_device *dev)
{
    if (!dev->in_use) return;
    /* Don't arm if there's no room. */
    if (rx_ring_free(dev) < (int)sizeof(dev->rx_xfer)) return;

    int xfer_len = (int)sizeof(dev->rx_xfer);
    if (dev->max_packet_in && dev->max_packet_in < xfer_len)
        xfer_len = dev->max_packet_in;

    int got = xhci_bulk_poll_in(dev->xdev, dev->ep_in,
                                dev->rx_xfer, xfer_len);
    if (got > 0) {
        rx_ring_push(dev, dev->rx_xfer, got);
    }
    /* got == 0: armed or still pending. got < 0: stall — leave for now. */
}

void usb_cdc_pump(void)
{
    for (int i = 0; i < cdc_count; i++) {
        cdc_pump_one(&cdc_devs[i]);
    }
}

/* ── Recv (non-blocking) ─────────────────────────────────────── */

int usb_cdc_recv(struct usb_cdc_device *dev, void *out, int max)
{
    if (!dev || !dev->in_use || !out || max <= 0) return -1;

    /* Pump first so freshly arrived bytes are available right away. */
    cdc_pump_one(dev);

    uint8_t *o = (uint8_t *)out;
    int n = 0;
    while (n < max && dev->rx_head != dev->rx_tail) {
        o[n++] = dev->rx_buf[dev->rx_head];
        dev->rx_head = (dev->rx_head + 1) & (USB_CDC_RX_RING_SIZE - 1);
    }
    return n;
}
