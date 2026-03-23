/*
 * Zeos — UDP
 */

#ifndef ZEOS_NET_UDP_H
#define ZEOS_NET_UDP_H

#include "net.h"

struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

/* Send a UDP datagram */
int udp_send(struct ipv4_addr dst, uint16_t src_port, uint16_t dst_port,
             const void *data, uint16_t len);

/* Process incoming UDP (called by ip_process) */
void udp_process(const void *frame, uint16_t len);

/* Register a callback for a local port */
typedef void (*udp_recv_fn)(struct ipv4_addr src, uint16_t src_port,
                            const void *data, uint16_t len);
int udp_bind(uint16_t port, udp_recv_fn callback);

#endif /* ZEOS_NET_UDP_H */
