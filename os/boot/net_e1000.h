/*
 * Zeos -- Intel e1000/e1000e NIC driver
 *
 * Supports the classic Intel 82540EM (QEMU) and i217/i219 (real HW).
 * The CN60 Chromebox has an Intel i219-LM, which uses a compatible
 * register interface.
 *
 * Polling-only for Alpha -- no interrupt handler.
 */

#ifndef ZEOS_NET_E1000_H
#define ZEOS_NET_E1000_H

#include "net.h"

/* Initialize e1000 NIC (PCI vendor 0x8086, class 0x02).
 * Returns 0 on success, -1 if no Intel NIC found. */
int e1000_init(void);

/* Send a raw Ethernet frame. Returns 0 on success, -1 on error. */
int e1000_send(const void *frame, uint16_t len);

/* Receive a raw Ethernet frame (polling).
 * Returns frame length, or 0 if nothing available. */
int e1000_recv(void *frame, uint16_t max_len);

/* Get the device MAC address. */
void e1000_get_mac(struct mac_addr *mac);

#endif /* ZEOS_NET_E1000_H */
