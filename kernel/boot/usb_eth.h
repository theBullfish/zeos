/*
 * Zeos -- USB CDC-ECM Ethernet class driver
 *
 * Implements USB Communication Device Class -- Ethernet Control Model
 * (CDC-ECM, USB-IF spec 1.2). Supports any USB ethernet adapter that
 * speaks ECM, which includes:
 *   - QEMU's `usb-net` device (CDC-ECM by default)
 *   - Many real-world USB-A and USB-C ethernet dongles (Apple A1277,
 *     Cable Matters 202023, generic Realtek-RT8152 in CDC mode, etc.)
 *
 * Detection: walks the configuration descriptor of every xHCI-attached
 * device looking for a Communication interface (class 0x02, sub 0x06)
 * paired with a Data interface (class 0x0A) that exposes Bulk IN +
 * Bulk OUT endpoints. The MAC address comes from the iMACAddress
 * string descriptor referenced by the ECM Functional Descriptor.
 *
 * Plumbs into net.c as the LAST fallback in the NIC probe chain so a
 * laptop with no PCI NIC but a USB ethernet dongle still gets DHCP.
 */

#ifndef ZEOS_USB_ETH_H
#define ZEOS_USB_ETH_H

#include "net.h"

/* Match the existing NIC driver API shape (see net_rtl8139.h). */

/* Returns 0 on success, -1 if no CDC-ECM device is present / probe fails. */
int usb_eth_init(void);

/* Send an ethernet frame. Returns bytes sent (== len) or -1 on error. */
int usb_eth_send(const void *frame, uint16_t len);

/* Non-blocking receive. Returns frame length on success, 0 if no frame
 * is currently buffered, -1 on hard error. */
int usb_eth_recv(void *frame, uint16_t max_len);

/* Copy the device's MAC into *mac. */
void usb_eth_get_mac(struct mac_addr *mac);

#endif /* ZEOS_USB_ETH_H */
