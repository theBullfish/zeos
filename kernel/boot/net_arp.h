/*
 * Zeos — ARP (Address Resolution Protocol)
 */

#ifndef ZEOS_NET_ARP_H
#define ZEOS_NET_ARP_H

#include "net.h"

/* ARP header */
struct arp_hdr {
    uint16_t        hw_type;      /* 1 = Ethernet */
    uint16_t        proto_type;   /* 0x0800 = IPv4 */
    uint8_t         hw_len;       /* 6 for Ethernet */
    uint8_t         proto_len;    /* 4 for IPv4 */
    uint16_t        operation;    /* 1 = request, 2 = reply */
    struct mac_addr  sender_mac;
    struct ipv4_addr sender_ip;
    struct mac_addr  target_mac;
    struct ipv4_addr target_ip;
} __attribute__((packed));

/* Process an incoming ARP packet */
void arp_process(const void *frame, uint16_t len);

/* Resolve an IP to a MAC. Blocking (sends request, waits for reply). */
int arp_resolve(struct ipv4_addr target, struct mac_addr *out);

#endif /* ZEOS_NET_ARP_H */
