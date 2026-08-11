/*
 * Zeos -- IPv6 stack header
 *
 * IPv6, ICMPv6, Neighbor Discovery, link-local, multicast.
 * Shares CHAIN_NET_TX/RX with v4 -- the L3 decap node in net_poll()
 * branches on EtherType (0x0800 v4 / 0x86DD v6).
 */

#ifndef ZEOS_NET_IPV6_H
#define ZEOS_NET_IPV6_H

#include "net.h"

/* ── Constants ─────────────────────────────────── */

#define ETH_TYPE_IPV6        0x86DD

/* IPv6 next-header values */
#define IPV6_NH_HOPOPT        0
#define IPV6_NH_TCP           6
#define IPV6_NH_UDP          17
#define IPV6_NH_ICMPV6       58
#define IPV6_NH_NO_NEXT      59

/* ICMPv6 message types */
#define ICMPV6_ECHO_REQUEST     128
#define ICMPV6_ECHO_REPLY       129
#define ICMPV6_ROUTER_SOLICIT   133
#define ICMPV6_ROUTER_ADVERT    134
#define ICMPV6_NEIGHBOR_SOLICIT 135
#define ICMPV6_NEIGHBOR_ADVERT  136

/* ND option types */
#define ND_OPT_SOURCE_LL_ADDR   1
#define ND_OPT_TARGET_LL_ADDR   2
#define ND_OPT_PREFIX_INFO      3
#define ND_OPT_MTU              5

/* ── Types ─────────────────────────────────────── */

struct ipv6_addr {
    uint8_t bytes[16];
} __attribute__((packed));

/* IPv6 fixed header (40 bytes) */
struct ipv6_hdr {
    uint32_t          ver_tc_fl;     /* 4-bit version | 8-bit TC | 20-bit flow */
    uint16_t          payload_len;   /* big-endian */
    uint8_t           next_header;
    uint8_t           hop_limit;
    struct ipv6_addr  src;
    struct ipv6_addr  dst;
} __attribute__((packed));

/* ICMPv6 fixed header */
struct icmpv6_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint32_t reserved;
} __attribute__((packed));

/* ── Configuration ─────────────────────────────── */

#define IPV6_NEIGH_CACHE_SIZE 32
#define IPV6_ADDR_MAX         4    /* link-local + SLAAC + DHCPv6 + room */

struct ipv6_neigh_entry {
    struct ipv6_addr ip;
    struct mac_addr  mac;
    uint64_t         last_tsc;
    int              valid;
};

struct ipv6_addr_entry {
    struct ipv6_addr addr;
    uint8_t          prefix_len;
    int              valid;
    int              is_dhcp;     /* 1 = from DHCPv6, 0 = link-local/SLAAC */
};

struct ipv6_state {
    struct ipv6_addr_entry  addrs[IPV6_ADDR_MAX];
    struct ipv6_addr        gateway;     /* router from RA */
    int                     have_gateway;
    int                     dhcpv6_active;   /* 1 = M-bit set, ran DHCPv6 */
    struct ipv6_neigh_entry neigh[IPV6_NEIGH_CACHE_SIZE];
};

extern struct ipv6_state g_net6;

/* ── Address helpers ───────────────────────────── */

static inline int ipv6_eq(struct ipv6_addr a, struct ipv6_addr b) {
    for (int i = 0; i < 16; i++) if (a.bytes[i] != b.bytes[i]) return 0;
    return 1;
}

static inline int ipv6_is_unspecified(struct ipv6_addr a) {
    for (int i = 0; i < 16; i++) if (a.bytes[i]) return 0;
    return 1;
}

static inline int ipv6_is_multicast(struct ipv6_addr a) {
    return a.bytes[0] == 0xFF;
}

static inline int ipv6_is_link_local(struct ipv6_addr a) {
    return a.bytes[0] == 0xFE && (a.bytes[1] & 0xC0) == 0x80;
}

/* All-nodes link-local: ff02::1 */
static inline struct ipv6_addr ipv6_all_nodes(void) {
    struct ipv6_addr a = {{0xFF,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,0x01}};
    return a;
}

/* All-routers link-local: ff02::2 */
static inline struct ipv6_addr ipv6_all_routers(void) {
    struct ipv6_addr a = {{0xFF,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,0x02}};
    return a;
}

/* Solicited-node multicast for target: ff02::1:ffXX:XXXX */
static inline struct ipv6_addr ipv6_solicited_node(struct ipv6_addr target) {
    struct ipv6_addr a = {{0xFF,0x02,0,0,0,0,0,0,0,0,0,0x01,0xFF,0,0,0}};
    a.bytes[13] = target.bytes[13];
    a.bytes[14] = target.bytes[14];
    a.bytes[15] = target.bytes[15];
    return a;
}

/* ── API ───────────────────────────────────────── */

/* Initialize: derive link-local from MAC via EUI-64, send RS, optionally DHCPv6.
 * Called from net_init() after the NIC is up and DHCPv4 has run. */
int  ipv6_init(void);
/* Async bring-up: non-blocking link-local + RS (call from net_init); SLAAC
 * completes via ipv6_service() ticks + the RX path. */
int  ipv6_start(void);
void ipv6_service(void);
int  ipv6_have_global(void);

/* Send a packet from our chosen source to dst, with given next-header proto.
 * Builds ethernet+IPv6 header, resolves dst MAC via ND or multicast mapping. */
int  ipv6_send(struct ipv6_addr dst, uint8_t next_header,
               const void *payload, uint16_t len);

/* Process an incoming ethernet+IPv6 frame. Called from net_poll() when
 * ethertype == 0x86DD. */
void ipv6_process(const void *frame, uint16_t len);

/* ICMPv6 echo. Returns TSC delta, 0 = timeout. */
uint64_t ipv6_ping(struct ipv6_addr target);

/* ND: resolve target IPv6 -> MAC. Returns 0 on success. */
int  ipv6_neigh_resolve(struct ipv6_addr target, struct mac_addr *out);

/* Get our preferred source IPv6 address for a given destination.
 * Prefers DHCPv6/SLAAC global for global dst, link-local for link-local dst. */
struct ipv6_addr ipv6_pick_source(struct ipv6_addr dst);

/* Choose dst MAC for a given IPv6 dst (multicast mapping or ND lookup). */
int  ipv6_resolve_dst_mac(struct ipv6_addr dst, struct mac_addr *out);

/* Format an IPv6 address into a canonical text string. dst must be >=40 bytes. */
void ipv6_format(struct ipv6_addr a, char *dst);

/* Parse an IPv6 address string. Returns 0 on success. */
int  ipv6_parse(const char *s, struct ipv6_addr *out);

/* Selftest helper: prints the IPv6 selftest line. */
void ipv6_print_selftest_line(void);

#endif /* ZEOS_NET_IPV6_H */
