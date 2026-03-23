/*
 * Zeos — virtio-net PCI driver
 *
 * Uses the virtio legacy (0.9) PCI interface.
 * Provides raw Ethernet frame send/receive.
 */

#ifndef ZEOS_NET_VIRTIO_H
#define ZEOS_NET_VIRTIO_H

#include "net.h"

/* Initialize virtio-net device. Returns 0 on success, -1 if not found. */
int virtio_net_init(void);

/* Send a raw Ethernet frame. Returns 0 on success. */
int virtio_net_send(const void *data, uint16_t len);

/* Poll-receive a frame. Returns frame length, or 0 if none available. */
int virtio_net_recv(void *buf, uint16_t max_len);

/* Get the device MAC address. */
void virtio_net_get_mac(struct mac_addr *mac);

#endif /* ZEOS_NET_VIRTIO_H */
