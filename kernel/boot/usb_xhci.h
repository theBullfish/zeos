/*
 * Zeos -- xHCI (USB 3.x) Host Controller Driver
 *
 * Polling-only minimal xHCI 1.2 driver.
 *  - Discovers controller via PCI (class 0x0C, subclass 0x03, prog-if 0x30)
 *  - Resets + starts controller
 *  - Allocates DCBAA, command ring, event ring + ERST
 *  - Detects connected devices on root-hub ports
 *  - Performs Enable Slot + Address Device + GET_DESCRIPTOR(Device)
 *
 * Out of scope: hubs (beyond root hub), isochronous, MSI-X, USB class
 * drivers (HID/MSC/CDC). Those will be follow-up commits.
 */

#ifndef ZEOS_USB_XHCI_H
#define ZEOS_USB_XHCI_H

#include <stdint.h>
#include "usb.h"

#define XHCI_MAX_DEVICES 8

struct xhci_device {
    int      slot_id;       /* xHCI slot ID, 0 = unused */
    int      port;          /* root-hub port (1-based) */
    int      speed;         /* USB_SPEED_* */
    uint8_t  address;       /* assigned USB address */
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t bcd_usb;
    uint8_t  max_packet_size0;
    /* Per-device structures (allocated at slot enable) */
    void    *input_ctx;     /* Input Context (page-aligned) */
    void    *device_ctx;    /* Output / Device Context (page-aligned) */
    void    *ep0_ring;      /* EP0 transfer ring (page-aligned) */
    uint32_t ep0_enqueue;   /* enqueue index into ep0_ring */
    uint32_t ep0_cycle;     /* producer cycle bit */
};

/*
 * Discover and initialize the first xHCI controller.
 * Returns 0 on success, negative on failure or when no controller present.
 */
int xhci_init(void);

/* Lookup a previously enumerated device by VID/PID (0/0 = wildcard). */
struct xhci_device *xhci_find_device(uint16_t vid, uint16_t pid);

/*
 * Issue a control transfer on EP0.
 * Returns number of data bytes transferred (>=0) on success, negative on error.
 */
int xhci_control_transfer(struct xhci_device *dev,
                          struct usb_setup_packet *setup,
                          void *buf, int max_len);

/*
 * Bulk transfer stub. Currently returns -1 (no bulk endpoint setup yet).
 * Kept in the public API so class drivers can compile against the final
 * shape; real implementation arrives with the HID/MSC commits.
 */
int xhci_bulk_transfer(struct xhci_device *dev, int ep, void *buf, int len, int in);

/* Number of xHCI devices currently tracked. */
int xhci_device_count(void);

/* Get device by index (0..count-1), or NULL. */
struct xhci_device *xhci_get_device(int index);

#endif /* ZEOS_USB_XHCI_H */
