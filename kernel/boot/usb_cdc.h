/*
 * Zeos -- USB CDC ACM Class Driver
 *
 * Detects USB serial devices (Arduino, ESP32 native USB, Pi Pico,
 * any standards-conformant CDC-ACM widget) and exposes a tiny
 * send / non-blocking recv API plus a kernel-managed handle table
 * the shell wires up to "usb-serial", "cdc-send", "cdc-recv".
 *
 *   - Standards CDC-ACM:    interface class 0x02 / subclass 0x02
 *                           data interface class 0x0A
 *   - Pseudo-CDC / single-interface vendor flavors are caught by
 *     a small VID/PID match table (Pi Pico stdio, ESP32-S2/S3 USB,
 *     etc.) and treated as "interface 0 has the bulk pair".
 */

#ifndef ZEOS_USB_CDC_H
#define ZEOS_USB_CDC_H

#include <stdint.h>
#include "usb_xhci.h"

#define USB_CDC_MAX_DEVICES   4
#define USB_CDC_RX_RING_SIZE  512

struct usb_cdc_device {
    int                  in_use;
    struct xhci_device  *xdev;
    int                  iface_data;     /* data-interface number */
    uint8_t              ep_in;          /* bulk IN  ep address (0x8x) */
    uint8_t              ep_out;         /* bulk OUT ep address (0x0x) */
    uint16_t             max_packet_in;
    uint16_t             max_packet_out;
    /* RX ring (filled by polling). Producer = irq-poll path,
     * consumer = cdc_recv. */
    uint8_t              rx_buf[USB_CDC_RX_RING_SIZE];
    uint32_t             rx_head;        /* read index */
    uint32_t             rx_tail;        /* write index */
    /* Transient buffer the xHCI driver writes into for the next bulk-IN
     * report. Lives in the device struct so its address is stable. */
    uint8_t              rx_xfer[64];
    int                  rx_armed;       /* non-zero if a TRB is queued */
    char                 name[16];       /* e.g. "ttyUSB0" */
};

/* Initialize CDC layer: scan all xHCI devices and bind each USB serial
 * device. Returns number of CDC devices bound. Safe to call once after
 * xhci_init(). */
int usb_cdc_init(void);

/* Number of bound CDC devices. */
int usb_cdc_count(void);

/* Get device by index (0..count-1) — for shell listing. */
struct usb_cdc_device *usb_cdc_get(int idx);

/* Send raw bytes to a CDC device. Returns bytes written or -1. */
int usb_cdc_send(struct usb_cdc_device *dev, const void *buf, int len);

/* Drain the RX ring into out (up to max bytes). Non-blocking; returns
 * 0 if nothing buffered. Negative on error. Internally also pumps the
 * bulk-IN endpoint so calling this regularly keeps data flowing. */
int usb_cdc_recv(struct usb_cdc_device *dev, void *out, int max);

/* Pump all CDC devices: drain bulk-IN events into rx_buf rings. Called
 * by usb_cdc_recv automatically; exposed so a periodic kernel task can
 * also call it. */
void usb_cdc_pump(void);

#endif
