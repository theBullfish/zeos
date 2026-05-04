/*
 * Zeos -- USB Hub class driver
 *
 * Walks every device that xhci_init() enumerated, identifies hubs (device
 * class 0x09 or interface class 0x09), reads the hub descriptor, and
 * enumerates each downstream port. New devices found behind a hub are
 * appended to the xHCI device list via xhci_address_hub_device(), which
 * sets the Slot Context Route String + parent-hub TT fields the
 * controller needs to route traffic through the hub.
 *
 * Recurses one level for now (root → hub → device). A hub discovered
 * behind another hub is brought up to Address Device + descriptor read,
 * but its downstream ports are not walked — that's documented as future
 * work in usb_hub.c. Single-tier external hub plus the internal hub
 * QEMU exposes between root and the keyboard/mouse/disk both work.
 */

#ifndef ZEOS_USB_HUB_H
#define ZEOS_USB_HUB_H

#include <stdint.h>

/* Returns the number of new devices addressed behind hubs. */
int usb_hub_init(void);

#endif /* ZEOS_USB_HUB_H */
