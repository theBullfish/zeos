/*
 * Zeos -- USB Hub class driver
 *
 * USB 2.0 spec ch. 11 / USB 3.x spec ch. 10. Detects hubs (class 0x09),
 * powers downstream ports, polls Get Port Status, resets each populated
 * port, then asks the xHCI driver to address the new downstream device
 * with the correct Route String + parent-hub TT context.
 *
 * Full multi-tier recursion: when a downstream device reports class
 * 0x09 (hub), we recurse into it and enumerate its downstream ports
 * too. The xHCI 1.2 route string is 20 bits = 5 nibbles = 5 tiers max,
 * so we refuse to descend past tier 5 and log a depth-limit message.
 * Each hub's bPwrOn2PwrGood is honoured for the power-on settle delay.
 */

#include "usb_hub.h"
#include "usb.h"
#include "usb_xhci.h"
#include "kprint.h"

extern void *memset(void *s, int c, unsigned long n);

/* xHCI 1.2 §4.5.2 — route string is 20 bits, one nibble per tier. */
#define USB_HUB_MAX_TIER 5

/* Tracks the deepest tier reached during the most recent walk. */
static int g_usb_hub_max_tier = 0;

/* ── Hub class constants (USB 2.0 §11.24, USB 3.x §10.16) ─────── */
#define USB_CLASS_HUB                   0x09

/* Hub-class descriptor types */
#define HUB_DT_HUB_USB2                 0x29  /* USB 2.0 hub descriptor */
#define HUB_DT_HUB_USB3                 0x2A  /* SuperSpeed hub descriptor */

/* Hub-class-specific requests (bRequest values) */
#define HUB_REQ_GET_STATUS              0x00
#define HUB_REQ_CLEAR_FEATURE           0x01
#define HUB_REQ_SET_FEATURE             0x03
#define HUB_REQ_GET_DESCRIPTOR          0x06
#define HUB_REQ_SET_DESCRIPTOR          0x07
#define HUB_REQ_CLEAR_TT_BUFFER         0x08
#define HUB_REQ_RESET_TT                0x09
#define HUB_REQ_GET_TT_STATE            0x0A
#define HUB_REQ_STOP_TT                 0x0B

/* Port features (wValue for SET_FEATURE / CLEAR_FEATURE on a port) */
#define PORT_CONNECTION                 0
#define PORT_ENABLE                     1
#define PORT_SUSPEND                    2
#define PORT_OVER_CURRENT               3
#define PORT_RESET                      4
#define PORT_POWER                      8
#define PORT_LOW_SPEED                  9
#define C_PORT_CONNECTION               16
#define C_PORT_ENABLE                   17
#define C_PORT_SUSPEND                  18
#define C_PORT_OVER_CURRENT             19
#define C_PORT_RESET                    20

/* Port status bits (low 16 bits of GET_STATUS-on-port response) */
#define PORTSTAT_CONNECTION             (1U << 0)
#define PORTSTAT_ENABLE                 (1U << 1)
#define PORTSTAT_SUSPEND                (1U << 2)
#define PORTSTAT_OVER_CURRENT           (1U << 3)
#define PORTSTAT_RESET                  (1U << 4)
#define PORTSTAT_POWER                  (1U << 8)
#define PORTSTAT_LOW_SPEED              (1U << 9)
#define PORTSTAT_HIGH_SPEED             (1U << 10)

/* bmRequestType values used by hub-class control transfers. The hub itself
 * is the recipient for Get Hub Descriptor; a port is "Other" recipient. */
#define RT_HUB_GET                      (USB_DIR_IN  | USB_TYPE_CLASS | USB_RECIP_DEVICE)
#define RT_HUB_SET                      (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_DEVICE)
#define RT_PORT_GET                     (USB_DIR_IN  | USB_TYPE_CLASS | 0x03 /* Other */)
#define RT_PORT_SET                     (USB_DIR_OUT | USB_TYPE_CLASS | 0x03 /* Other */)

/* USB 2.0 hub descriptor (variable length; we only need the head). */
struct hub_descriptor {
    uint8_t  bDescLength;
    uint8_t  bDescriptorType;   /* 0x29 */
    uint8_t  bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPwrOn2PwrGood;    /* in 2 ms units */
    uint8_t  bHubContrCurrent;
    /* DeviceRemovable + PortPwrCtrlMask follow; unused here */
} __attribute__((packed));

/* ── Spin helper (no timer in early boot) ─────────────────────── */
static void hub_delay_ms(uint32_t ms)
{
    /* Calibrated against the spin loop xhci.c uses: ~100k iters/ms on the
     * Z13 reference. This is generous; cost is bounded by hub tier × ports. */
    for (uint32_t i = 0; i < ms; i++)
        for (volatile uint32_t j = 0; j < 100000; j++) ;
}

/* ── Hub-class control wrappers ───────────────────────────────── */
static int hub_get_descriptor(struct xhci_device *hub, void *buf, int len)
{
    struct usb_setup_packet sp;
    sp.bmRequestType = RT_HUB_GET;
    sp.bRequest      = HUB_REQ_GET_DESCRIPTOR;
    /* For USB2 hubs the spec allows wValue = 0; some firmware wants the
     * descriptor type in the high byte. Use type | index = 0x2900 which
     * matches every implementation in the wild. */
    sp.wValue        = ((uint16_t)HUB_DT_HUB_USB2 << 8);
    sp.wIndex        = 0;
    sp.wLength       = (uint16_t)len;
    return xhci_control_transfer(hub, &sp, buf, len);
}

static int hub_get_port_status(struct xhci_device *hub, int port,
                               uint16_t *status, uint16_t *change)
{
    uint8_t buf[4];
    memset(buf, 0, sizeof(buf));
    struct usb_setup_packet sp;
    sp.bmRequestType = RT_PORT_GET;
    sp.bRequest      = HUB_REQ_GET_STATUS;
    sp.wValue        = 0;
    sp.wIndex        = (uint16_t)port;
    sp.wLength       = 4;
    int r = xhci_control_transfer(hub, &sp, buf, 4);
    if (r < 4) return -1;
    if (status) *status = (uint16_t)(buf[0] | (buf[1] << 8));
    if (change) *change = (uint16_t)(buf[2] | (buf[3] << 8));
    return 0;
}

static int hub_set_port_feature(struct xhci_device *hub, int port,
                                int feature)
{
    struct usb_setup_packet sp;
    sp.bmRequestType = RT_PORT_SET;
    sp.bRequest      = HUB_REQ_SET_FEATURE;
    sp.wValue        = (uint16_t)feature;
    sp.wIndex        = (uint16_t)port;
    sp.wLength       = 0;
    int r = xhci_control_transfer(hub, &sp, 0, 0);
    return (r < 0) ? -1 : 0;
}

static int hub_clear_port_feature(struct xhci_device *hub, int port,
                                  int feature)
{
    struct usb_setup_packet sp;
    sp.bmRequestType = RT_PORT_SET;
    sp.bRequest      = HUB_REQ_CLEAR_FEATURE;
    sp.wValue        = (uint16_t)feature;
    sp.wIndex        = (uint16_t)port;
    sp.wLength       = 0;
    int r = xhci_control_transfer(hub, &sp, 0, 0);
    return (r < 0) ? -1 : 0;
}

/* ── Map USB hub PORTSTAT speed bits to xHCI USB_SPEED_* ─────── */
static int speed_from_portstat(uint16_t s)
{
    /* USB 2.0: bit 9 = LowSpeed, bit 10 = HighSpeed. Neither set = Full. */
    if (s & PORTSTAT_LOW_SPEED)  return USB_SPEED_LOW;
    if (s & PORTSTAT_HIGH_SPEED) return USB_SPEED_HIGH;
    return USB_SPEED_FULL;
}

/* ── Reset one downstream port and address the new device ─────── */
static int hub_reset_port(struct xhci_device *hub, int port,
                          int *out_speed)
{
    /* Start the port reset. */
    if (hub_set_port_feature(hub, port, PORT_RESET) != 0) return -1;

    /* Poll for the reset to complete. USB 2.0 spec allows up to ~50 ms. */
    uint16_t status = 0, change = 0;
    int done = 0;
    for (int i = 0; i < 50; i++) {
        hub_delay_ms(10);
        if (hub_get_port_status(hub, port, &status, &change) != 0) continue;
        if (change & (1U << 4) /* C_PORT_RESET */) { done = 1; break; }
        if (status & PORTSTAT_ENABLE) { done = 1; break; }
    }
    if (!done) return -1;

    /* Acknowledge C_PORT_RESET so future polls see clean state. */
    hub_clear_port_feature(hub, port, C_PORT_RESET);

    /* Re-read status to get the post-reset speed bits. */
    if (hub_get_port_status(hub, port, &status, &change) != 0) return -1;
    if (!(status & PORTSTAT_CONNECTION) || !(status & PORTSTAT_ENABLE))
        return -1;

    if (out_speed) *out_speed = speed_from_portstat(status);

    /* Recovery delay (USB 2.0 §7.1.7.5 TRSTRCY = 10 ms). */
    hub_delay_ms(10);
    return 0;
}

/* ── Determine if a device exposes hub class ──────────────────── */
static int device_is_hub(struct xhci_device *d)
{
    if (!d || d->slot_id == 0) return 0;
    if (d->dev_class == USB_CLASS_HUB) return 1;
    /* Some hubs report bDeviceClass=0 and only flag Hub at the interface
     * level (rare on real hubs, but spec-legal). Walk the configuration
     * descriptor as a fallback. */
    if (d->dev_class != 0) return 0;
    uint8_t cfg[256];
    memset(cfg, 0, sizeof(cfg));
    int got = xhci_get_config_descriptor(d, cfg, sizeof(cfg));
    if (got < 9) return 0;
    int total = cfg[2] | (cfg[3] << 8);
    if (total > got) total = got;
    int i = cfg[0];
    while (i + 2 <= total) {
        uint8_t bLen = cfg[i];
        uint8_t bType = cfg[i + 1];
        if (bLen < 2) break;
        if (bType == USB_DT_INTERFACE && i + 9 <= total) {
            if (cfg[i + 5] == USB_CLASS_HUB) return 1;
        }
        i += bLen;
    }
    return 0;
}

/* ── Walk one hub: power ports, reset connected ports, address ──
 *
 * `tier` is the hub's own tier (1 for a hub plugged into a root port,
 * 2 for a hub plugged into a tier-1 hub, etc.). Devices the hub
 * addresses live at tier+1. The xHCI route-string caps at tier 5; if
 * we'd produce a child past tier 5 we refuse to descend further. */
static int enumerate_hub(struct xhci_device *hub, int tier)
{
    if (tier > g_usb_hub_max_tier) g_usb_hub_max_tier = tier;

    /* Configure the hub if its class driver needs it. SET_CONFIGURATION
     * is harmless if the device was already configured. */
    if (hub->configuration == 0) {
        uint8_t cfg[64];
        memset(cfg, 0, sizeof(cfg));
        int g = xhci_get_config_descriptor(hub, cfg, sizeof(cfg));
        if (g >= 6) {
            uint8_t cv = cfg[5]; /* bConfigurationValue */
            xhci_set_configuration(hub, cv);
        }
    }

    /* Read hub descriptor. */
    uint8_t hbuf[16];
    memset(hbuf, 0, sizeof(hbuf));
    int hd = hub_get_descriptor(hub, hbuf, sizeof(hbuf));
    if (hd < 7) {
        kputs("[hub] GET_DESCRIPTOR(Hub) failed (");
        kput_dec(hd < 0 ? 0 : (uint32_t)hd);
        kputs(" bytes)\n");
        return 0;
    }

    struct hub_descriptor desc;
    memset(&desc, 0, sizeof(desc));
    desc.bDescLength       = hbuf[0];
    desc.bDescriptorType   = hbuf[1];
    desc.bNbrPorts         = hbuf[2];
    desc.wHubCharacteristics = (uint16_t)(hbuf[3] | (hbuf[4] << 8));
    desc.bPwrOn2PwrGood    = hbuf[5];
    desc.bHubContrCurrent  = hbuf[6];

    int nports = desc.bNbrPorts;
    int ttt = (desc.wHubCharacteristics >> 5) & 0x3; /* TT think time */
    int mtt = (desc.wHubCharacteristics >> 7) & 0x1;
    int pwr_on_ms = (int)desc.bPwrOn2PwrGood * 2;
    if (pwr_on_ms < 10) pwr_on_ms = 10;

    kputs("[hub] hub slot=");
    kput_dec(hub->slot_id);
    kputs(" ports=");
    kput_dec(nports);
    kputs(" pwr_on_ms=");
    kput_dec(pwr_on_ms);
    kputs("\n");

    /* Mark this slot as a hub on the controller side so it will route
     * downstream Address Device commands. */
    if (xhci_mark_hub(hub, nports, mtt, ttt) != 0) {
        kputs("[hub] mark_hub failed; downstream addressing unlikely to work\n");
    }

    /* Power every port; some hubs ship with PPC=1 and require this. */
    for (int p = 1; p <= nports; p++) {
        hub_set_port_feature(hub, p, PORT_POWER);
    }
    hub_delay_ms((uint32_t)pwr_on_ms);

    int new_devices = 0;
    for (int p = 1; p <= nports; p++) {
        uint16_t s = 0, c = 0;
        if (hub_get_port_status(hub, p, &s, &c) != 0) continue;

        /* Acknowledge any stale change bits. */
        if (c & (1U << 0)) hub_clear_port_feature(hub, p, C_PORT_CONNECTION);
        if (c & (1U << 1)) hub_clear_port_feature(hub, p, C_PORT_ENABLE);
        if (c & (1U << 4)) hub_clear_port_feature(hub, p, C_PORT_RESET);

        if (!(s & PORTSTAT_CONNECTION)) continue;

        /* Settle: spec says 100 ms after CCS for power. */
        hub_delay_ms(100);

        int speed = USB_SPEED_FULL;
        if (hub_reset_port(hub, p, &speed) != 0) {
            kputs("[hub]  port ");
            kput_dec(p);
            kputs(" reset failed\n");
            continue;
        }

        kputs("[hub]  port ");
        kput_dec(p);
        kputs(" device speed=");
        kput_dec(speed);
        kputs("\n");

        struct xhci_device *child =
            xhci_address_hub_device(hub, p, speed);
        if (!child) {
            kputs("[hub]  port ");
            kput_dec(p);
            kputs(" address failed\n");
            continue;
        }
        new_devices++;

        /* Track the tier the child lives at. */
        int child_tier = tier + 1;
        if (child_tier > g_usb_hub_max_tier)
            g_usb_hub_max_tier = child_tier;

        /* If the child is itself a hub, recurse — but only as long as
         * its downstream children would still fit inside the 5-nibble
         * route string. A hub at tier 5 cannot address further
         * devices because tier-6 nibbles don't exist. */
        if (device_is_hub(child)) {
            if (child_tier >= USB_HUB_MAX_TIER) {
                kputs("USB: hub depth limit reached\n");
            } else {
                new_devices += enumerate_hub(child, child_tier);
            }
        }
    }
    return new_devices;
}

/* ── Public entry point ───────────────────────────────────────── */
int usb_hub_init(void)
{
    int total = 0;
    g_usb_hub_max_tier = 0;
    /* Snapshot the pre-existing device count so we don't iterate into
     * children we just appended. */
    int n = xhci_device_count();
    for (int i = 0; i < n; i++) {
        struct xhci_device *d = xhci_get_device(i);
        if (!d) continue;
        if (!device_is_hub(d)) continue;
        /* A hub picked up by xhci_init() sits at tier 1 — root port is
         * tier 0, the hub itself is tier 1, its downstream devices at
         * tier 2, and so on. */
        total += enumerate_hub(d, 1);
    }
    if (total > 0) {
        kputs("[hub] ");
        kput_dec(total);
        kputs(" downstream device(s) addressed, max tier=");
        kput_dec(g_usb_hub_max_tier);
        kputs("\n");
    }
    return total;
}

int usb_hub_max_tier(void)
{
    return g_usb_hub_max_tier;
}

int usb_hub_enumerate_downstream(struct xhci_device *hub, int tier)
{
    if (!hub) return 0;
    if (tier < 1) tier = 1;
    if (tier > USB_HUB_MAX_TIER) {
        kputs("USB: hub depth limit reached\n");
        return 0;
    }
    return enumerate_hub(hub, tier);
}
