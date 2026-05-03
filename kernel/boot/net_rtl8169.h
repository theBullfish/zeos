/*
 * Zeos — Realtek RTL8169/RTL8168 Gigabit NIC driver
 *
 * The dominant gigabit Ethernet chip in 2008-2018 consumer
 * laptops and desktops (Asus, Acer, Dell, MSI, Gigabyte, etc).
 * Descriptor-ring based, MMIO-driven.
 *
 * PCI vendor 0x10EC, device IDs 0x8169, 0x8168, 0x8161, 0x8136.
 */

#ifndef ZEOS_NET_RTL8169_H
#define ZEOS_NET_RTL8169_H

#include "net.h"

/* Returns 0 on success, -1 if no RTL8169-family NIC found. */
int rtl8169_init(void);

int  rtl8169_send(const void *frame, uint16_t len);
int  rtl8169_recv(void *frame, uint16_t max_len);
void rtl8169_get_mac(struct mac_addr *mac);

#endif /* ZEOS_NET_RTL8169_H */
