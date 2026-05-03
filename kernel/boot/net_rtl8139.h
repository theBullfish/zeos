/*
 * Zeos — Realtek RTL8139 NIC driver
 *
 * The most common 100M Ethernet chip in 2002-2010 consumer laptops
 * and desktops. Trivial register set (I/O port based), no descriptor
 * rings — single linear RX ring + 4 TX descriptors. Polling-only.
 *
 * PCI vendor 0x10EC, device 0x8139.
 */

#ifndef ZEOS_NET_RTL8139_H
#define ZEOS_NET_RTL8139_H

#include "net.h"

/* Returns 0 on success, -1 if no RTL8139 found. */
int rtl8139_init(void);

int rtl8139_send(const void *frame, uint16_t len);
int rtl8139_recv(void *frame, uint16_t max_len);
void rtl8139_get_mac(struct mac_addr *mac);

#endif /* ZEOS_NET_RTL8139_H */
