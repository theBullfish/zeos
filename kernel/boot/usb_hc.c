/*
 * Zeos -- USB host-controller dispatch layer.
 *
 * Owns the public xhci_* API symbols used by all USB class drivers
 * (HID / MSC / CDC / Ethernet / RTL8188EU / hub). For each call we look at
 * dev->hc_kind and route to the matching xhci_real_* or ehci_real_* impl.
 *
 * xhci_init() here is the boot-path entry point: it tries the real xHCI
 * driver first, and on failure (or zero controllers found) falls through
 * to ehci_init(). xhci_device_count() / xhci_get_device() present a unified
 * device list across whichever HC came up. Only one HC is expected to be
 * live on a given system; if both come up, devices from both are visible.
 */

#include "usb_hc.h"
#include "usb_xhci.h"
#include "usb_ehci.h"
#include "kprint.h"

static int xhci_up = 0;
static int ehci_up = 0;

int xhci_init(void)
{
    int rc = xhci_real_init();
    if (rc == 0 && xhci_real_device_count() > 0) {
        xhci_up = 1;
        return 0;
    }
    /* xHCI absent or empty -- try EHCI for legacy systems */
    if (rc == 0) {
        /* xHCI initialized but found no devices; we still mark it as up so
         * the dispatcher can route to it if a device shows up later. */
        xhci_up = 1;
    }
    int er = ehci_init();
    if (er == 0) {
        ehci_up = 1;
        return 0;
    }
    return xhci_up ? 0 : -1;
}

int xhci_device_count(void)
{
    int c = 0;
    if (xhci_up) c += xhci_real_device_count();
    if (ehci_up) c += ehci_device_count();
    return c;
}

struct xhci_device *xhci_get_device(int index)
{
    if (index < 0) return 0;
    if (xhci_up) {
        int xn = xhci_real_device_count();
        if (index < xn) return xhci_real_get_device(index);
        index -= xn;
    }
    if (ehci_up) {
        if (index < ehci_device_count()) return ehci_get_device(index);
    }
    return 0;
}

struct xhci_device *xhci_find_device(uint16_t vid, uint16_t pid)
{
    if (xhci_up) {
        struct xhci_device *d = xhci_real_find_device(vid, pid);
        if (d) return d;
    }
    if (ehci_up) {
        struct xhci_device *d = ehci_find_device(vid, pid);
        if (d) return d;
    }
    return 0;
}

#define DISPATCH(dev, fn, ...) \
    do { \
        if (!(dev)) return -1; \
        if ((dev)->hc_kind == HC_KIND_EHCI) return ehci_real_##fn(__VA_ARGS__); \
        return xhci_real_##fn(__VA_ARGS__); \
    } while (0)

int xhci_control_transfer(struct xhci_device *dev,
                          struct usb_setup_packet *setup,
                          void *buf, int max_len)
{ DISPATCH(dev, control_transfer, dev, setup, buf, max_len); }

int xhci_get_config_descriptor(struct xhci_device *dev, void *out, int max)
{ DISPATCH(dev, get_config_descriptor, dev, out, max); }

int xhci_set_configuration(struct xhci_device *dev, uint8_t cfgv)
{ DISPATCH(dev, set_configuration, dev, cfgv); }

int xhci_setup_bulk_endpoint(struct xhci_device *dev,
                             uint8_t ep_addr, uint16_t max_packet)
{ DISPATCH(dev, setup_bulk_endpoint, dev, ep_addr, max_packet); }

int xhci_bulk_transfer(struct xhci_device *dev, int ep_addr,
                       void *buf, int len, int in)
{ DISPATCH(dev, bulk_transfer, dev, ep_addr, buf, len, in); }

int xhci_bulk_poll_in(struct xhci_device *dev, int ep_addr,
                      void *buf, int len)
{ DISPATCH(dev, bulk_poll_in, dev, ep_addr, buf, len); }

int xhci_setup_interrupt_in(struct xhci_device *dev,
                            uint8_t ep_addr, uint16_t max_packet,
                            uint8_t interval)
{ DISPATCH(dev, setup_interrupt_in, dev, ep_addr, max_packet, interval); }

int xhci_interrupt_poll(struct xhci_device *dev, void *buf, int len)
{ DISPATCH(dev, interrupt_poll, dev, buf, len); }

int xhci_interrupt_transfer(struct xhci_device *dev, void *buf, int len)
{ DISPATCH(dev, interrupt_transfer, dev, buf, len); }

/* Isochronous IN — xHCI only. UVC + USB-audio capture won't work over
 * the legacy EHCI path; that's an honest constraint, not a stub. */
int xhci_iso_setup(struct xhci_device *dev, uint8_t ep_addr,
                   uint16_t mps, uint8_t pkts_per_frame, uint8_t interval,
                   xhci_iso_cb_t cb, void *user)
{
    extern int xhci_real_iso_setup(struct xhci_device *, uint8_t,
                                   uint16_t, uint8_t, uint8_t,
                                   xhci_iso_cb_t, void *);
    if (!dev) return -1;
    if (dev->hc_kind == HC_KIND_EHCI) return -1;
    return xhci_real_iso_setup(dev, ep_addr, mps, pkts_per_frame,
                               interval, cb, user);
}

int xhci_iso_start(struct xhci_device *dev)
{
    extern int xhci_real_iso_start(struct xhci_device *);
    if (!dev || dev->hc_kind == HC_KIND_EHCI) return -1;
    return xhci_real_iso_start(dev);
}

int xhci_iso_stop(struct xhci_device *dev)
{
    extern int xhci_real_iso_stop(struct xhci_device *);
    if (!dev || dev->hc_kind == HC_KIND_EHCI) return -1;
    return xhci_real_iso_stop(dev);
}

int xhci_iso_pump(struct xhci_device *dev)
{
    extern int xhci_real_iso_pump(struct xhci_device *);
    if (!dev || dev->hc_kind == HC_KIND_EHCI) return -1;
    return xhci_real_iso_pump(dev);
}

/* Hub support — only the xHCI driver implements these. */
struct xhci_device *xhci_address_hub_device(struct xhci_device *parent,
                                            int parent_port, int speed)
{
    if (!parent) return 0;
    if (parent->hc_kind == HC_KIND_EHCI) return 0;
    return xhci_real_address_hub_device(parent, parent_port, speed);
}

int xhci_mark_hub(struct xhci_device *hub, int num_ports, int mtt,
                  int think_time)
{
    if (!hub) return -1;
    if (hub->hc_kind == HC_KIND_EHCI) return -1;
    return xhci_real_mark_hub(hub, num_ports, mtt, think_time);
}
