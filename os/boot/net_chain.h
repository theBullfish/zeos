/*
 * Zeos -- Chain-native networking glue
 *
 * Generic chain layer for the NIC subsystem. Individual NIC drivers
 * (e1000, rtl8139, rtl8169, virtio-net, usb-eth) register themselves
 * as the hardware_dma backend via net_hw_ops. The TX and RX chains
 * are otherwise driver-agnostic.
 *
 * Pipelines (per the chain contract):
 *   TX: frame_request  -> l2_encap -> mac_filter -> hardware_dma
 *   RX: hardware_dma   -> mac_filter -> l2_decap -> frame_delivery
 */

#ifndef ZEOS_NET_CHAIN_H
#define ZEOS_NET_CHAIN_H

#include <stdint.h>

/* The hardware_dma backend each NIC driver provides.
 *   tx:      transmit a finished ethernet frame, returns 0 on success.
 *   rx_poll: pull at most one frame, returns length (0 if empty). */
struct net_hw_ops {
    int (*tx)(const void *frame, uint16_t len);
    int (*rx_poll)(void *buf, uint16_t max);
    const char *name;
};

/* Wire the active NIC driver as the hardware_dma backend. Call from
 * net_init() after picking the driver. */
void net_chain_set_hw(const struct net_hw_ops *ops);

/* Register the four-node TX and four-node RX chains. Called from
 * chain_registry_init(). Returns 0 on success. */
int net_chain_register(int parent_id);

/* Resolve functions exposed for chain_registry.c. */
void net_tx_request_resolve     (void *self, void *input, void *output);
void net_tx_l2_encap_resolve    (void *self, void *input, void *output);
void net_tx_mac_filter_resolve  (void *self, void *input, void *output);
void net_tx_hardware_dma_resolve(void *self, void *input, void *output);

void net_rx_hardware_dma_resolve(void *self, void *input, void *output);
void net_rx_mac_filter_resolve  (void *self, void *input, void *output);
void net_rx_l2_decap_resolve    (void *self, void *input, void *output);
void net_rx_frame_delivery_resolve(void *self, void *input, void *output);

/* Compat-shim entry points the rest of the kernel calls.
 *
 * Both go through chain_resolve() so every TX and every RX flows
 * through the typed pipeline. The pre-chain function pointers
 * (net_drv_send / net_drv_recv) are pointed at these so all
 * existing callers (ARP, IP, DHCP, etc.) get the chain treatment
 * for free. */
int  net_chain_send(const void *frame, uint16_t len);
int  net_chain_recv(void *buf, uint16_t max);

#endif /* ZEOS_NET_CHAIN_H */
