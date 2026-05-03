/*
 * Zeos -- USB CDC-ECM Ethernet class driver
 *
 * See usb_eth.h for high-level overview.
 *
 * CDC-ECM enumeration walk:
 *   1. Walk every xHCI-attached device.
 *   2. For each, fetch the configuration descriptor blob.
 *   3. Find an interface with bInterfaceClass == 0x02 (Communication),
 *      bInterfaceSubClass == 0x06 (ECM). The next interface (or one
 *      identified by the Union FD) with class 0x0A is the data
 *      interface, which carries the bulk endpoints.
 *   4. The ECM Functional Descriptor (subtype 0x0F) holds iMACAddress,
 *      a string descriptor index whose UTF-16 ASCII payload is 12 hex
 *      digits of the MAC.
 *   5. Switch the data interface to alt setting 1 (operational —
 *      alt 0 is "zero bandwidth" with no endpoints on most adapters,
 *      including QEMU usb-net). Issue SET_INTERFACE.
 *   6. Configure the bulk IN and bulk OUT endpoints on xHCI.
 *
 * Receive model:
 *   - Keep one bulk-IN URB always armed via xhci_bulk_poll_in. On every
 *     usb_eth_recv() call, check for completion; if a frame arrived,
 *     copy it out and re-arm.
 *
 * Send model:
 *   - usb_eth_send() does a synchronous bulk_transfer on the OUT EP.
 *     For 1500-byte frames at HS this completes in microseconds in
 *     QEMU; the network stack already calls send synchronously.
 */

#include "usb_eth.h"
#include "usb_xhci.h"
#include "usb.h"
#include "kprint.h"

extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *s, int c, unsigned long n);

/* ── CDC class constants ─────────────────────────────────────── */
#define USB_CLASS_CDC               0x02
#define USB_CLASS_CDC_DATA          0x0A
#define CDC_SUBCLASS_ECM            0x06

/* CDC functional descriptor subtypes (CDC 1.2 §5.2.3) */
#define CDC_FD_HEADER               0x00
#define CDC_FD_UNION                0x06
#define CDC_FD_ECM                  0x0F

/* USB descriptor types specific to interfaces/endpoints (already in usb.h: 4, 5) */
/* Functional descriptors use type 0x24 (Class-specific Interface) */
#define USB_DT_CS_INTERFACE         0x24

/* SET_INTERFACE request */
#define USB_REQ_SET_INTERFACE       0x0B

/* ── Driver state ────────────────────────────────────────────── */
static struct {
    int ready;
    struct xhci_device *dev;
    struct mac_addr mac;
    uint8_t bulk_in;     /* endpoint address (0x80 | n) */
    uint8_t bulk_out;    /* endpoint address (0x00 | n) */
    uint16_t mps_in;
    uint16_t mps_out;
    uint8_t  data_iface;
    uint8_t  data_alt;
    /* Persistent bulk-IN buffer (kept armed across calls) */
    uint8_t  rx_buf[2048];
    int      rx_pending;     /* 1 once first arm is done */
} eth;

/* ── Small helpers ───────────────────────────────────────────── */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

/* Read a USB string descriptor (16-bit UTF-16LE) and ASCII-fold it.
 * Returns ASCII length stored in out (no NUL), or -1 on error. */
static int read_string_descriptor(struct xhci_device *dev, uint8_t index,
                                  char *out, int max)
{
    if (!index) return -1;
    uint8_t buf[256];
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    sp.bRequest = USB_REQ_GET_DESCRIPTOR;
    sp.wValue = (USB_DT_STRING << 8) | index;
    sp.wIndex = 0x0409;  /* English (US) */
    sp.wLength = sizeof(buf);
    int got = xhci_control_transfer(dev, &sp, buf, sizeof(buf));
    if (got < 2) return -1;
    int total = buf[0];
    if (total > got) total = got;
    if (total < 2) return -1;
    int n = 0;
    for (int i = 2; i + 1 < total && n < max - 1; i += 2) {
        out[n++] = (char)buf[i];  /* low byte = ASCII for ASCII-only strings */
    }
    out[n] = 0;
    return n;
}

/* Parse 12 hex chars at *s into 6 MAC bytes. Returns 0 on success. */
static int parse_mac_string(const char *s, int len, struct mac_addr *out)
{
    if (len < 12) return -1;
    for (int i = 0; i < 6; i++) {
        int hi = hex_nibble(s[2 * i]);
        int lo = hex_nibble(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out->b[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* ── CDC-ECM probe of one device ─────────────────────────────── */

/*
 * Walk one device's configuration descriptor looking for the ECM
 * Communication+Data interface pair. Fills the eth state on success.
 * Returns 0 on success, -1 if this device isn't a CDC-ECM adapter.
 */
static int probe_device(struct xhci_device *dev)
{
    /* Pull config descriptor (one page is plenty for any ECM device) */
    static uint8_t cfg[1024];
    int total = xhci_get_config_descriptor(dev, cfg, sizeof(cfg));
    if (total < 9) return -1;
    /* Configuration descriptor (9 bytes): wTotalLength @ [2..3],
     * bNumInterfaces @ [4], bConfigurationValue @ [5]. */
    int wTotal = cfg[2] | ((int)cfg[3] << 8);
    if (wTotal > total) wTotal = total;
    uint8_t bConfigValue = cfg[5];

    /* Walk descriptors looking for: a CDC Communication interface with
     * subclass ECM, then its associated Data interface (class 0x0A) and
     * the Bulk IN+OUT endpoints inside that data interface's alt
     * setting that has them. */
    int comm_iface = -1;
    uint8_t mac_str_idx = 0;
    int data_iface = -1;
    /* Track current interface while walking */
    int cur_iface = -1;
    int cur_alt = 0;
    int cur_class = -1, cur_sub = -1;
    /* Track the data interface alt that has bulk endpoints */
    int data_alt_with_eps = -1;
    uint8_t bulk_in = 0, bulk_out = 0;
    uint16_t mps_in = 0, mps_out = 0;

    int off = 0;
    while (off + 2 <= wTotal) {
        uint8_t bLen = cfg[off];
        uint8_t bType = cfg[off + 1];
        if (bLen < 2 || off + bLen > wTotal) break;

        if (bType == USB_DT_INTERFACE && bLen >= 9) {
            cur_iface = cfg[off + 2];   /* bInterfaceNumber */
            cur_alt   = cfg[off + 3];   /* bAlternateSetting */
            cur_class = cfg[off + 5];
            cur_sub   = cfg[off + 6];
            if (cur_class == USB_CLASS_CDC && cur_sub == CDC_SUBCLASS_ECM &&
                comm_iface < 0) {
                comm_iface = cur_iface;
            }
            /* If we already know which data iface to look for, and this
             * interface alt setting has class 0x0A, treat this alt as a
             * candidate. We'll commit once we see endpoints inside. */
        } else if (bType == USB_DT_CS_INTERFACE && bLen >= 3 &&
                   cur_class == USB_CLASS_CDC && cur_sub == CDC_SUBCLASS_ECM) {
            uint8_t subtype = cfg[off + 2];
            if (subtype == CDC_FD_ECM && bLen >= 13) {
                /* ECM Functional Descriptor:
                 *   bFunctionLength, bDescriptorType, bDescriptorSubtype,
                 *   iMACAddress (1B), bmEthernetStatistics (4B),
                 *   wMaxSegmentSize (2B), wNumberMCFilters (2B),
                 *   bNumberPowerFilters (1B). */
                mac_str_idx = cfg[off + 3];
            } else if (subtype == CDC_FD_UNION && bLen >= 5) {
                /* Union FD: master iface @ [3], slave iface(s) start @ [4].
                 * We only need slave[0] as the data interface. */
                if (data_iface < 0)
                    data_iface = cfg[off + 4];
            }
        } else if (bType == USB_DT_ENDPOINT && bLen >= 7 &&
                   cur_class == USB_CLASS_CDC_DATA) {
            /* Endpoint descriptor: bEndpointAddress @ [2],
             * bmAttributes @ [3] (low 2 bits: 2 = Bulk),
             * wMaxPacketSize @ [4..5]. */
            uint8_t ep_addr = cfg[off + 2];
            uint8_t attrs   = cfg[off + 3];
            uint16_t mps    = cfg[off + 4] | ((uint16_t)cfg[off + 5] << 8);
            mps &= 0x07FF;  /* low 11 bits is the actual size */
            if ((attrs & 0x03) == 0x02) {  /* bulk */
                if (data_iface < 0) {
                    /* No Union FD specified; first data iface we find
                     * with bulk EPs is our data iface. */
                    data_iface = cur_iface;
                }
                if (cur_iface == data_iface) {
                    if (data_alt_with_eps < 0) data_alt_with_eps = cur_alt;
                    if (cur_alt == data_alt_with_eps) {
                        if (ep_addr & 0x80) {
                            bulk_in = ep_addr;
                            mps_in = mps;
                        } else {
                            bulk_out = ep_addr;
                            mps_out = mps;
                        }
                    }
                }
            }
        }
        off += bLen;
    }

    if (comm_iface < 0 || data_iface < 0 ||
        bulk_in == 0 || bulk_out == 0) return -1;

    /* Read MAC from string descriptor */
    char mac_s[64];
    int slen = read_string_descriptor(dev, mac_str_idx, mac_s, sizeof(mac_s));
    struct mac_addr mac;
    memset(&mac, 0, sizeof(mac));
    if (slen >= 12) {
        if (parse_mac_string(mac_s, slen, &mac) != 0) {
            /* Fallback: leave zero; some devices stash MAC differently */
        }
    }

    /* If MAC still zero, fabricate a locally-administered one based on
     * VID/PID so the network stack at least has a stable identity. */
    int zero = 1;
    for (int i = 0; i < 6; i++) if (mac.b[i]) { zero = 0; break; }
    if (zero) {
        mac.b[0] = 0x02;  /* locally administered, unicast */
        mac.b[1] = 0xCD;
        mac.b[2] = 0xEC;
        mac.b[3] = (uint8_t)(dev->vendor_id >> 8);
        mac.b[4] = (uint8_t)(dev->vendor_id & 0xFF);
        mac.b[5] = (uint8_t)(dev->product_id & 0xFF);
    }

    /* SET_CONFIGURATION first if not yet done. The xHCI driver does
     * NOT issue SET_CONFIGURATION during enumeration; we own that. */
    if (dev->configuration != bConfigValue) {
        if (xhci_set_configuration(dev, bConfigValue) < 0) {
            kputs("[usb-eth] SET_CONFIGURATION failed\n");
            return -1;
        }
    }

    /* SET_INTERFACE on data iface to its operational alt setting. For
     * QEMU usb-net the operational alt is 1 (alt 0 is the zero-bandwidth
     * placeholder per the CDC-ECM spec). */
    if (data_alt_with_eps != 0) {
        struct usb_setup_packet sp;
        sp.bmRequestType = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE;
        sp.bRequest = USB_REQ_SET_INTERFACE;
        sp.wValue = (uint16_t)data_alt_with_eps;
        sp.wIndex = (uint16_t)data_iface;
        sp.wLength = 0;
        if (xhci_control_transfer(dev, &sp, 0, 0) < 0) {
            kputs("[usb-eth] SET_INTERFACE failed\n");
            return -1;
        }
    }

    /* Configure bulk IN + OUT on xHCI */
    if (xhci_setup_bulk_endpoint(dev, bulk_out, mps_out) < 0) {
        kputs("[usb-eth] bulk OUT setup failed\n");
        return -1;
    }
    if (xhci_setup_bulk_endpoint(dev, bulk_in, mps_in) < 0) {
        kputs("[usb-eth] bulk IN setup failed\n");
        return -1;
    }

    eth.dev = dev;
    eth.mac = mac;
    eth.bulk_in = bulk_in;
    eth.bulk_out = bulk_out;
    eth.mps_in = mps_in;
    eth.mps_out = mps_out;
    eth.data_iface = (uint8_t)data_iface;
    eth.data_alt = (uint8_t)data_alt_with_eps;
    eth.rx_pending = 0;
    eth.ready = 1;

    kputs("[usb-eth] CDC-ECM ready: VID=");
    kput_hex(dev->vendor_id);
    kputs(" PID=");
    kput_hex(dev->product_id);
    kputs(" iface=");
    kput_dec(data_iface);
    kputs(" alt=");
    kput_dec(data_alt_with_eps);
    kputs(" in=");
    kput_hex(bulk_in);
    kputs("/");
    kput_dec(mps_in);
    kputs(" out=");
    kput_hex(bulk_out);
    kputs("/");
    kput_dec(mps_out);
    kputs(" mac=");
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        if (i) kputc(':');
        kputc(hexd[(mac.b[i] >> 4) & 0xF]);
        kputc(hexd[mac.b[i] & 0xF]);
    }
    kputs("\n");
    return 0;
}

/* ── Public API ──────────────────────────────────────────────── */

int usb_eth_init(void)
{
    memset(&eth, 0, sizeof(eth));
    int n = xhci_device_count();
    for (int i = 0; i < n; i++) {
        struct xhci_device *d = xhci_get_device(i);
        if (!d || d->slot_id == 0) continue;
        if (probe_device(d) == 0) return 0;
    }
    return -1;
}

int usb_eth_send(const void *frame, uint16_t len)
{
    if (!eth.ready) return -1;
    /* xhci_bulk_transfer expects a writable buffer pointer; the data is
     * read by hardware so const-cast is safe. */
    int sent = xhci_bulk_transfer(eth.dev, eth.bulk_out,
                                  (void *)(uintptr_t)frame, (int)len, 0);
    if (sent < 0) return -1;
    /* CDC-ECM frames whose length is an exact multiple of the bulk OUT
     * max-packet-size require a Zero-Length Packet to mark the end. */
    if (sent == (int)len && eth.mps_out > 0 &&
        (len % eth.mps_out) == 0 && len > 0) {
        /* Send a zero-length packet (a Normal TRB with length 0). The
         * controller handles this correctly. */
        uint8_t dummy = 0;
        xhci_bulk_transfer(eth.dev, eth.bulk_out, &dummy, 0, 0);
    }
    return sent;
}

int usb_eth_recv(void *frame, uint16_t max_len)
{
    if (!eth.ready) return 0;
    /* Use our persistent rx_buf as the IN target; copy out on completion. */
    int r = xhci_bulk_poll_in(eth.dev, eth.bulk_in,
                              eth.rx_buf, sizeof(eth.rx_buf));
    if (r <= 0) return r < 0 ? 0 : 0;  /* nothing or error -> non-blocking 0 */

    int n = r;
    if (n > (int)max_len) n = (int)max_len;
    memcpy(frame, eth.rx_buf, (uint64_t)n);
    return n;
}

void usb_eth_get_mac(struct mac_addr *mac)
{
    if (!eth.ready) {
        memset(mac, 0, sizeof(*mac));
        return;
    }
    *mac = eth.mac;
}
