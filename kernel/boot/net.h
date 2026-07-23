/*
 * Zeos — Networking Stack
 *
 * Shared types and configuration for the network subsystem.
 * QEMU SLIRP provides DHCP, DNS, and NAT — we hardcode the addresses.
 */

#ifndef ZEOS_NET_H
#define ZEOS_NET_H

#include <stdint.h>

/* ── Constants ────────────────────────────────── */

#define NET_MTU           1500
#define NET_FRAME_MAX     (NET_MTU + 14)    /* Ethernet header = 14 bytes */
#define NET_BUF_COUNT     16                /* Static packet buffer pool */
#define NET_BUF_SIZE      2048

/* Ethernet types */
#define ETH_TYPE_ARP      0x0806
#define ETH_TYPE_IP       0x0800

/* IP protocols */
#define IP_PROTO_ICMP     1
#define IP_PROTO_TCP      6
#define IP_PROTO_UDP      17

/* ── Types ────────────────────────────────────── */

struct mac_addr {
    uint8_t b[6];
} __attribute__((packed));

struct ipv4_addr {
    uint8_t b[4];
} __attribute__((packed));

/* Ethernet header (14 bytes) */
struct eth_hdr {
    struct mac_addr dst;
    struct mac_addr src;
    uint16_t        ethertype;    /* Big-endian */
} __attribute__((packed));

/* IPv4 header (20 bytes, no options) */
struct ipv4_hdr {
    uint8_t         ver_ihl;      /* Version (4) + IHL (5) = 0x45 */
    uint8_t         tos;
    uint16_t        total_len;    /* Big-endian */
    uint16_t        id;
    uint16_t        flags_frag;
    uint8_t         ttl;
    uint8_t         protocol;
    uint16_t        checksum;
    struct ipv4_addr src;
    struct ipv4_addr dst;
} __attribute__((packed));

/* Network packet buffer */
struct net_buf {
    uint8_t  data[NET_BUF_SIZE];
    uint16_t len;           /* Total valid bytes */
};

/* Global network configuration */
struct net_config {
    struct mac_addr  mac;       /* Our MAC (from virtio) */
    struct ipv4_addr ip;        /* 10.0.2.15 */
    struct ipv4_addr gateway;   /* 10.0.2.2 */
    struct ipv4_addr netmask;   /* 255.255.255.0 */
    struct ipv4_addr dns;       /* 10.0.2.3 */
    int              up;        /* Is the network up? */
};

extern struct net_config g_net;

/* ── Driver dispatch (set by net_init based on hardware) ── */

/* Function pointers to active NIC driver (virtio or e1000) */
extern int  (*net_drv_send)(const void *data, uint16_t len);
extern int  (*net_drv_recv)(void *buf, uint16_t max_len);
extern void (*net_drv_get_mac)(struct mac_addr *mac);

/* ── Byte-swap helpers ────────────────────────── */

static inline uint16_t htons(uint16_t v) { return (v >> 8) | (v << 8); }
static inline uint16_t ntohs(uint16_t v) { return (v >> 8) | (v << 8); }
static inline uint32_t htonl(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

/* ── IP address helpers ───────────────────────── */

static inline uint32_t ip_to_u32(struct ipv4_addr a) {
    return ((uint32_t)a.b[0] << 24) | ((uint32_t)a.b[1] << 16) |
           ((uint32_t)a.b[2] << 8) | a.b[3];
}

static inline struct ipv4_addr u32_to_ip(uint32_t v) {
    struct ipv4_addr a;
    a.b[0] = (v >> 24) & 0xFF;
    a.b[1] = (v >> 16) & 0xFF;
    a.b[2] = (v >> 8) & 0xFF;
    a.b[3] = v & 0xFF;
    return a;
}

static inline int ip_eq(struct ipv4_addr a, struct ipv4_addr b) {
    return a.b[0] == b.b[0] && a.b[1] == b.b[1] &&
           a.b[2] == b.b[2] && a.b[3] == b.b[3];
}

static inline int mac_eq(struct mac_addr a, struct mac_addr b) {
    return a.b[0] == b.b[0] && a.b[1] == b.b[1] && a.b[2] == b.b[2] &&
           a.b[3] == b.b[3] && a.b[4] == b.b[4] && a.b[5] == b.b[5];
}

/* ── Checksum ─────────────────────────────────── */

/* Internet checksum (ones-complement of ones-complement sum) */
static inline uint16_t net_checksum(const void *data, int len)
{
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len == 1)
        sum += *(const uint8_t *)p;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ── Master init/poll ─────────────────────────── */

/* Initialize the entire network stack. Returns 0 on success. */
int net_init(void);

/* Poll for incoming packets and dispatch them. Non-blocking. */
void net_poll(void);

/* Poll with timeout (ms). Returns when a packet arrives or timeout. */
void net_poll_wait(uint32_t timeout_ms);

/* Per-tick network service (RX dispatch + async DHCP), called by the scheduler. */
void net_service(void);

#endif /* ZEOS_NET_H */
