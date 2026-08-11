/*
 * Zeos -- Internal host-controller dispatch glue.
 *
 * Forwards the public xhci_* API (used by all USB class drivers) to the
 * underlying real driver based on dev->hc_kind. Class drivers continue to
 * include only "usb_xhci.h" and call xhci_*; this header is consumed by
 * usb_hc.c and by the per-HC drivers internally.
 */
#ifndef ZEOS_USB_HC_H
#define ZEOS_USB_HC_H

#include "usb_xhci.h"

/* xHCI "real" implementations live in usb_xhci.c and are renamed so the
 * public xhci_* names are owned by the dispatcher in usb_hc.c. */
int xhci_real_init(void);
int xhci_real_device_count(void);
struct xhci_device *xhci_real_get_device(int index);
struct xhci_device *xhci_real_find_device(uint16_t vid, uint16_t pid);

int xhci_real_control_transfer(struct xhci_device *dev,
                               struct usb_setup_packet *setup,
                               void *buf, int max_len);
int xhci_real_get_config_descriptor(struct xhci_device *dev, void *out, int max);
int xhci_real_set_configuration(struct xhci_device *dev, uint8_t cfgv);

int xhci_real_setup_bulk_endpoint(struct xhci_device *dev,
                                  uint8_t ep_addr, uint16_t max_packet);
int xhci_real_bulk_transfer(struct xhci_device *dev, int ep_addr,
                            void *buf, int len, int in);
int xhci_real_bulk_poll_in(struct xhci_device *dev, int ep_addr,
                           void *buf, int len);

int xhci_real_setup_interrupt_in(struct xhci_device *dev,
                                 uint8_t ep_addr, uint16_t max_packet,
                                 uint8_t interval);
int xhci_real_interrupt_poll(struct xhci_device *dev, void *buf, int len);
int xhci_real_interrupt_transfer(struct xhci_device *dev, void *buf, int len);

/* Hub support — only the xHCI driver implements these. EHCI legacy
 * hubs go through the EHCI host-controller's own bus (ehci_init walks
 * its own ports) so the dispatcher just rejects EHCI here. */
struct xhci_device *xhci_real_address_hub_device(struct xhci_device *parent,
                                                 int parent_port, int speed);
int xhci_real_mark_hub(struct xhci_device *hub, int num_ports, int mtt,
                       int think_time);

#endif /* ZEOS_USB_HC_H */
