/*
 * Zeos -- EHCI 1.0 (USB 2.0) Host Controller Driver
 *
 * Polling-only minimal EHCI driver. Used on legacy systems (pre-2012 era)
 * that ship without an xHCI controller. Same per-device data model as the
 * xHCI driver -- class drivers (HID/MSC/CDC/USB-Eth) consume the unified
 * `struct xhci_device` shape via the usb_hc.c dispatcher, so the EHCI path
 * fills the same fields and is selected at runtime by dev->hc_kind.
 *
 * PCI:  class 0x0C, subclass 0x03, prog-if 0x20.
 *
 * Implements:
 *   - HCRESET via USBCMD, wait for clear
 *   - Periodic Frame List (1024 entries, 4 KiB aligned)
 *   - Asynchronous List Head + dummy Queue Head
 *   - USBINTR=0 (polling), CONFIGFLAG=1 (own ports)
 *   - Per-port reset via PORTSC.PortReset (50 ms)
 *   - Address Device via SET_ADDRESS standard control transfer
 *   - Control transfers (SETUP/DATA/STATUS via QH + qTDs)
 *   - Bulk transfers (qTDs on a per-endpoint bulk Queue Head)
 *   - Interrupt-IN endpoints via the Periodic Frame List
 */

#ifndef ZEOS_USB_EHCI_H
#define ZEOS_USB_EHCI_H

#include <stdint.h>
#include "usb.h"
#include "usb_xhci.h"   /* shares the unified `struct xhci_device` shape */

int ehci_init(void);

/* Per-device list */
int ehci_device_count(void);
struct xhci_device *ehci_get_device(int index);
struct xhci_device *ehci_find_device(uint16_t vid, uint16_t pid);

/* "real" implementations called from the usb_hc.c dispatcher when
 * dev->hc_kind == HC_KIND_EHCI. Class drivers should *not* call these
 * directly -- use the xhci_* public API which dispatches transparently. */
int ehci_real_control_transfer(struct xhci_device *dev,
                               struct usb_setup_packet *setup,
                               void *buf, int max_len);
int ehci_real_get_config_descriptor(struct xhci_device *dev, void *out, int max);
int ehci_real_set_configuration(struct xhci_device *dev, uint8_t config_value);

int ehci_real_setup_bulk_endpoint(struct xhci_device *dev,
                                  uint8_t ep_addr, uint16_t max_packet);
int ehci_real_bulk_transfer(struct xhci_device *dev, int ep_addr,
                            void *buf, int len, int in);
int ehci_real_bulk_poll_in(struct xhci_device *dev, int ep_addr,
                           void *buf, int len);

int ehci_real_setup_interrupt_in(struct xhci_device *dev,
                                 uint8_t ep_addr, uint16_t max_packet,
                                 uint8_t interval);
int ehci_real_interrupt_poll(struct xhci_device *dev, void *buf, int len);
int ehci_real_interrupt_transfer(struct xhci_device *dev, void *buf, int len);

#endif /* ZEOS_USB_EHCI_H */
