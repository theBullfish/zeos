/*
 * Zeos -- Bluetooth USB transport (HCI USB Transport Layer)
 *
 * Implements the Bluetooth Core Spec Vol 4 Part B "HCI USB Transport"
 * mapping over the existing xHCI driver. A BT controller is detected by
 * its standard USB class triple:
 *   bDeviceClass    = 0xE0  (Wireless)
 *   bDeviceSubClass = 0x01  (RF)
 *   bDeviceProtocol = 0x01  (Bluetooth)
 *
 * Endpoint layout (interface 0, mandated by the spec):
 *   Endpoint 0   — control. HCI commands ride on a class-specific control
 *                  transfer (bmRequestType 0x20, bRequest 0x00, value 0,
 *                  index 0, data = HCI command packet).
 *   EP1 IN  intr — HCI Events (poll for asynchronous events / replies).
 *   EP2 OUT bulk — HCI ACL Data OUT.
 *   EP2 IN  bulk — HCI ACL Data IN.
 *   (EP3 ISOC — SCO/eSCO voice channels — not used here.)
 *
 * This file owns ONLY the transport. Command framing, event decoding,
 * BD_ADDR readback, inquiry / LE scan / connect lifecycle live in
 * bt_hci.c so the layering matches the spec.
 *
 * Foundation for OS_LITTLE_THINGS #16 BT pair UI.
 */

#ifndef ZEOS_BT_USB_H
#define ZEOS_BT_USB_H

#include <stdint.h>

/* USB Bluetooth class signature (Core Spec Vol 4 Part B §2.1). */
#define BT_USB_CLASS        0xE0
#define BT_USB_SUBCLASS     0x01
#define BT_USB_PROTOCOL     0x01

/* Maximum HCI command / event packet size we ever ship.
 * Spec caps event params at 255 + 2-byte header = 257 bytes; commands at
 * 255 + 3-byte header = 258. Round up for alignment headroom. */
#define BT_HCI_PKT_MAX      260

/* ACL packet ceiling. Spec allows up to 1023+4 for legacy controllers and
 * substantially larger for modern ones; 1024 is a safe envelope for the
 * boot UI (no A/V profiles yet). */
#define BT_ACL_PKT_MAX      1024

/* Forward decl from usb_xhci.h — opaque to this header. */
struct xhci_device;

/* Initialize the BT USB transport. Walks every enumerated xHCI device,
 * matches the E0/01/01 class triple on the device descriptor, and sets
 * up EP1 IN (interrupt, events) plus EP2 IN/OUT (bulk, ACL data). The
 * default control endpoint (EP0) is already addressed by xhci_init.
 *
 * Returns 0 if a controller was bound, -1 if none present. Both are
 * non-fatal for the OS — bt_usb_present() is the source of truth. */
int  bt_usb_init(void);

/* Has a BT controller been bound? */
int  bt_usb_present(void);

/* Send an HCI command. Wraps usb_setup_packet { 0x20, 0, 0, 0, len } and
 * issues a control-OUT transfer carrying the HCI command packet. The
 * caller-formatted packet (opcode + plen + params) is shipped verbatim.
 * Returns 0 on success, -1 on transport failure or no controller. */
int  bt_usb_send_command(const void *pkt, int len);

/* Drain one HCI event from EP1 IN. Non-blocking. Returns the event
 * length (>0) on receipt, 0 if no event was queued, -1 on error. */
int  bt_usb_poll_event(void *out, int max);

/* ACL bulk send / receive. ACL packets carry L2CAP data (future work).
 * Provided here so the transport surface is complete even though L2CAP
 * itself is not yet implemented. Returns bytes transferred or -1. */
int  bt_usb_acl_send(const void *pkt, int len);
int  bt_usb_acl_poll(void *out, int max);

/* Diagnostic: print VID:PID + endpoint addresses of the bound controller
 * via kprint. No-op when no controller is bound. */
void bt_usb_dump(void);

#endif /* ZEOS_BT_USB_H */
