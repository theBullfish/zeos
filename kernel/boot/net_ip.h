/*
 * Zeos — IPv4 + ICMP
 */

#ifndef ZEOS_NET_IP_H
#define ZEOS_NET_IP_H

#include "net.h"

/* Send an IPv4 packet. Handles ARP resolution and Ethernet framing. */
int ip_send(struct ipv4_addr dst, uint8_t protocol, const void *payload, uint16_t len);

/* Process an incoming IPv4 packet (demux to TCP/UDP/ICMP) */
void ip_process(const void *frame, uint16_t len);

/* Send ICMP echo request (ping). Returns RTT in TSC cycles, or 0 on timeout. */
uint64_t icmp_ping(struct ipv4_addr target);

#endif /* ZEOS_NET_IP_H */
