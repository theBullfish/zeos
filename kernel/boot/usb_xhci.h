/*
 * Zeos -- xHCI (USB 3.x) Host Controller Driver
 *
 * Polling-only minimal xHCI 1.2 driver.
 *  - Discovers controller via PCI (class 0x0C, subclass 0x03, prog-if 0x30)
 *  - Resets + starts controller
 *  - Allocates DCBAA, command ring, event ring + ERST
 *  - Detects connected devices on root-hub ports
 *  - Performs Enable Slot + Address Device + GET_DESCRIPTOR(Device)
 *  - Bulk + Interrupt-IN endpoint configuration for class drivers
 *    (CDC ACM, HID, Mass Storage, USB-Ethernet)
 */

#ifndef ZEOS_USB_XHCI_H
#define ZEOS_USB_XHCI_H

#include <stdint.h>
#include "usb.h"

/* Includes hub-attached devices: a typical laptop-internal hub plus an
 * external 4-port hub with kbd/mouse/disk needs ~10 slots. */
#define XHCI_MAX_DEVICES 16

/* Host controller kind tag — used by the usb_hc.c dispatch layer so the
 * xhci_* public API transparently routes to EHCI for legacy systems. */
#define HC_KIND_XHCI    1
#define HC_KIND_EHCI    2

/* Per-device interrupt-IN endpoint state (HID). */
struct xhci_int_ep {
    int      configured;
    uint8_t  ep_addr;
    uint8_t  dci;
    uint16_t max_packet;
    uint8_t  interval;
    void    *ring;
    uint64_t ring_phys;
    uint32_t enqueue;
    uint32_t cycle;
    uint64_t pending_trb_phys;
    void    *pending_buf;
    int      pending_len;
};

/* Bulk endpoint state (CDC, MSC, USB-Ethernet). */
struct xhci_bulk_ep {
    int      configured;
    uint8_t  ep_addr;
    uint8_t  dci;
    uint16_t max_packet;
    void    *ring;
    uint64_t ring_phys;
    uint32_t enqueue;
    uint32_t cycle;
    uint64_t pending_trb_phys;
    void    *pending_buf;
    int      pending_len;
};

#define XHCI_MAX_BULK_EPS 4

struct xhci_device {
    int      hc_kind;            /* HC_KIND_XHCI or HC_KIND_EHCI */
    void    *hc_priv;            /* HC-specific per-device state (EHCI) */
    int      slot_id;
    int      port;
    int      speed;
    uint8_t  address;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t bcd_usb;
    uint8_t  dev_class;
    uint8_t  dev_subclass;
    uint8_t  dev_protocol;
    uint8_t  max_packet_size0;
    uint8_t  configuration;
    uint32_t route_string;
    uint8_t  parent_hub_slot;
    uint8_t  parent_port;
    void    *input_ctx;
    void    *device_ctx;
    void    *ep0_ring;
    uint32_t ep0_enqueue;
    uint32_t ep0_cycle;
    struct xhci_int_ep int_in;
    struct xhci_bulk_ep bulk[XHCI_MAX_BULK_EPS];
};

int xhci_init(void);
struct xhci_device *xhci_find_device(uint16_t vid, uint16_t pid);

int xhci_control_transfer(struct xhci_device *dev,
                          struct usb_setup_packet *setup,
                          void *buf, int max_len);

int xhci_get_config_descriptor(struct xhci_device *dev, void *out, int max);
int xhci_set_configuration(struct xhci_device *dev, uint8_t config_value);

int xhci_setup_bulk_endpoint(struct xhci_device *dev,
                             uint8_t ep_addr, uint16_t max_packet);
int xhci_bulk_transfer(struct xhci_device *dev, int ep_addr,
                       void *buf, int len, int in);
int xhci_bulk_poll_in(struct xhci_device *dev, int ep_addr,
                      void *buf, int len);

int xhci_setup_interrupt_in(struct xhci_device *dev,
                            uint8_t ep_addr, uint16_t max_packet,
                            uint8_t interval);
int xhci_interrupt_poll(struct xhci_device *dev, void *buf, int len);
int xhci_interrupt_transfer(struct xhci_device *dev, void *buf, int len);

int xhci_device_count(void);
struct xhci_device *xhci_get_device(int index);

/* Hub support — implemented in usb_xhci.c, dispatched by usb_hc.c.
 *
 * xhci_address_hub_device() addresses a downstream device discovered
 * behind an existing hub. parent is the already-addressed hub device,
 * parent_port is the 1-based downstream hub port the new device is
 * attached to, and speed is the USB_SPEED_* code determined from the
 * hub's Get Port Status response. Returns the new xhci_device on
 * success (also appended to the device list), 0 on failure.
 *
 * xhci_mark_hub() updates an already-addressed device's slot context
 * to flag it as a hub (Hub bit + Number of Ports + MTT/TT think time)
 * via Evaluate Context. Required before the controller will route
 * Address Device commands through this hub. */
struct xhci_device *xhci_address_hub_device(struct xhci_device *parent,
                                            int parent_port, int speed);
int xhci_mark_hub(struct xhci_device *hub, int num_ports, int mtt,
                  int think_time);

#endif /* ZEOS_USB_XHCI_H */
