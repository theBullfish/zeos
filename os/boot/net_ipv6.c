/*
 * Zeos -- IPv6 stack: ND, DHCPv6, dual-stack TCP, AAAA DNS. Chains shared
 * with v4 -- CHAIN_NET_TX/RX carry both, the L3 decap node now branches
 * on EtherType (0x0800 v4 / 0x86DD v6).
 *
 * Implements:
 *   - Link-local self via EUI-64 from MAC
 *   - ICMPv6 echo (ping6)
 *   - Neighbor Solicitation / Advertisement (ND, replaces ARP)
 *   - Router Solicitation / Advertisement (auto-config, SLAAC + M-bit)
 *   - Multicast: solicited-node, all-nodes (ff02::1), all-routers (ff02::2)
 *   - 32-entry neighbor cache mirroring v4 ARP shape
 *   - Dispatch incoming ICMPv6 / UDP / TCP via next-header
 *
 * MasQ: every TX/RX still flows through CHAIN_NET_TX/RX -- we use the
 * same net_drv_send / net_drv_recv shims as the v4 stack. vault_version
 * bumps come from net_chain.c on each successful DMA.
 */

#include "net_ipv6.h"
#include "net.h"
#include "kprint.h"
#include "timer.h"   /* timer_read_tsc() — portable cycle counter */

/* External hooks for higher protocols */
extern void udp6_process(const struct ipv6_addr *src, const struct ipv6_addr *dst,
                         const void *udp_pkt, uint16_t udp_len);
extern void tcp6_process(const struct ipv6_addr *src, const struct ipv6_addr *dst,
                         const void *tcp_pkt, uint16_t tcp_len);
extern void dhcpv6_handle_advertise(const void *msg, uint16_t len,
                                    const struct ipv6_addr *server);
extern void dhcpv6_handle_reply(const void *msg, uint16_t len);

/* ── Module state ─────────────────────────────── */

struct ipv6_state g_net6;

static volatile int  s_echo_received = 0;
static uint64_t      s_echo_send_tsc = 0;
static uint64_t      s_echo_recv_tsc = 0;
static uint16_t      s_echo_id_seq   = 1;

static volatile int  s_neigh_resolved = 0;
static struct ipv6_addr s_neigh_target;

static int           s_initialised   = 0;

/* ── tiny libc ─────────────────────────────────── */

static uint64_t rdtsc6(void) {
    return timer_read_tsc();   /* HAL: was raw rdtsc */
}

static void mz(void *p, int n)            { uint8_t *b = p; for (int i=0;i<n;i++) b[i]=0; }
static void mc(void *d, const void *s, int n){ uint8_t *dd=d; const uint8_t *ss=s; for(int i=0;i<n;i++) dd[i]=ss[i]; }

/* ── Address helpers ──────────────────────────── */

static const char hexd[] = "0123456789abcdef";

void ipv6_format(struct ipv6_addr a, char *dst)
{
    /* Simple non-RFC5952 form: x:x:x:x:x:x:x:x with leading-zero trim,
     * but with a single :: collapse for the longest run of zeros. Good
     * enough for the shell. */
    uint16_t g[8];
    for (int i = 0; i < 8; i++)
        g[i] = ((uint16_t)a.bytes[i*2] << 8) | a.bytes[i*2+1];

    int best_start = -1, best_len = 0, cur_start = -1, cur_len = 0;
    for (int i = 0; i < 8; i++) {
        if (g[i] == 0) {
            if (cur_start < 0) { cur_start = i; cur_len = 1; }
            else cur_len++;
            if (cur_len > best_len) { best_len = cur_len; best_start = cur_start; }
        } else {
            cur_start = -1; cur_len = 0;
        }
    }
    if (best_len < 2) best_start = -1;  /* don't collapse a single zero */

    int o = 0;
    for (int i = 0; i < 8; i++) {
        if (i == best_start) {
            dst[o++] = ':';
            if (best_start == 0) dst[o++] = ':';
            i += best_len - 1;
            if (i == 7) dst[o++] = ':';
            continue;
        }
        if (i > 0) dst[o++] = ':';
        uint16_t v = g[i];
        int leading = 1;
        for (int s = 12; s >= 0; s -= 4) {
            uint8_t nyb = (v >> s) & 0xF;
            if (nyb == 0 && leading && s != 0) continue;
            leading = 0;
            dst[o++] = hexd[nyb];
        }
    }
    dst[o] = 0;
}

int ipv6_parse(const char *s, struct ipv6_addr *out)
{
    /* Minimal parser: handles full 8-group form and one '::' collapse. */
    uint16_t left[8] = {0}, right[8] = {0};
    int li = 0, ri = 0, in_right = 0;
    uint32_t cur = 0;
    int have_digit = 0;

    while (*s) {
        char c = *s;
        if (c == ':') {
            if (have_digit) {
                if (!in_right) {
                    if (li >= 8) return -1;
                    left[li++] = (uint16_t)cur;
                } else {
                    if (ri >= 8) return -1;
                    right[ri++] = (uint16_t)cur;
                }
                cur = 0; have_digit = 0;
            }
            if (s[1] == ':') {
                if (in_right) return -1;
                in_right = 1;
                s += 2;
                continue;
            }
            s++;
            continue;
        }
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return -1;
        cur = (cur << 4) | (uint32_t)v;
        if (cur > 0xFFFF) return -1;
        have_digit = 1;
        s++;
    }
    if (have_digit) {
        if (!in_right) {
            if (li >= 8) return -1;
            left[li++] = (uint16_t)cur;
        } else {
            if (ri >= 8) return -1;
            right[ri++] = (uint16_t)cur;
        }
    }
    if (!in_right && li != 8) return -1;
    if (in_right && (li + ri) > 8) return -1;

    uint16_t g[8] = {0};
    for (int i = 0; i < li; i++) g[i] = left[i];
    for (int i = 0; i < ri; i++) g[8 - ri + i] = right[i];
    for (int i = 0; i < 8; i++) {
        out->bytes[i*2]   = (uint8_t)(g[i] >> 8);
        out->bytes[i*2+1] = (uint8_t)(g[i] & 0xFF);
    }
    return 0;
}

/* ── Neighbor cache ───────────────────────────── */

static int neigh_lookup(struct ipv6_addr ip, struct mac_addr *out)
{
    for (int i = 0; i < IPV6_NEIGH_CACHE_SIZE; i++) {
        if (g_net6.neigh[i].valid && ipv6_eq(g_net6.neigh[i].ip, ip)) {
            *out = g_net6.neigh[i].mac;
            return 0;
        }
    }
    return -1;
}

static void neigh_add(struct ipv6_addr ip, struct mac_addr mac)
{
    for (int i = 0; i < IPV6_NEIGH_CACHE_SIZE; i++) {
        if (g_net6.neigh[i].valid && ipv6_eq(g_net6.neigh[i].ip, ip)) {
            g_net6.neigh[i].mac = mac;
            g_net6.neigh[i].last_tsc = rdtsc6();
            return;
        }
    }
    for (int i = 0; i < IPV6_NEIGH_CACHE_SIZE; i++) {
        if (!g_net6.neigh[i].valid) {
            g_net6.neigh[i].ip = ip;
            g_net6.neigh[i].mac = mac;
            g_net6.neigh[i].valid = 1;
            g_net6.neigh[i].last_tsc = rdtsc6();
            return;
        }
    }
    g_net6.neigh[0].ip = ip;
    g_net6.neigh[0].mac = mac;
    g_net6.neigh[0].last_tsc = rdtsc6();
}

/* ── Multicast MAC mapping ────────────────────── */

static struct mac_addr ipv6_multicast_mac(struct ipv6_addr a)
{
    /* RFC 2464: 33:33:XX:XX:XX:XX (low 32 bits of dst). */
    struct mac_addr m;
    m.b[0] = 0x33; m.b[1] = 0x33;
    m.b[2] = a.bytes[12];
    m.b[3] = a.bytes[13];
    m.b[4] = a.bytes[14];
    m.b[5] = a.bytes[15];
    return m;
}

/* ── Source address selection ─────────────────── */

struct ipv6_addr ipv6_pick_source(struct ipv6_addr dst)
{
    /* Link-local destination -> link-local source.
     * Otherwise prefer a global address (DHCPv6 or SLAAC). */
    int want_ll = ipv6_is_link_local(dst) ||
                  (dst.bytes[0] == 0xFF && (dst.bytes[1] & 0x0F) == 0x02);

    /* First pass: exact match by scope. */
    for (int i = 0; i < IPV6_ADDR_MAX; i++) {
        if (!g_net6.addrs[i].valid) continue;
        int is_ll = ipv6_is_link_local(g_net6.addrs[i].addr);
        if (is_ll == want_ll) return g_net6.addrs[i].addr;
    }
    /* Fallback: any valid address, link-local first. */
    for (int i = 0; i < IPV6_ADDR_MAX; i++) {
        if (g_net6.addrs[i].valid && ipv6_is_link_local(g_net6.addrs[i].addr))
            return g_net6.addrs[i].addr;
    }
    for (int i = 0; i < IPV6_ADDR_MAX; i++)
        if (g_net6.addrs[i].valid) return g_net6.addrs[i].addr;

    struct ipv6_addr z = {{0}};
    return z;
}

/* ── Pseudo-header checksum ───────────────────── */

static uint16_t ipv6_pseudo_checksum(const struct ipv6_addr *src,
                                     const struct ipv6_addr *dst,
                                     uint8_t next_header,
                                     const void *data, uint32_t len)
{
    uint32_t sum = 0;
    const uint16_t *p;

    p = (const uint16_t *)src->bytes;
    for (int i = 0; i < 8; i++) sum += p[i];
    p = (const uint16_t *)dst->bytes;
    for (int i = 0; i < 8; i++) sum += p[i];

    /* Length in network byte order, 32 bits split into two 16-bit halves */
    uint32_t lh = htonl(len);
    sum += (lh & 0xFFFF);
    sum += (lh >> 16);
    /* Next-header in network order, with leading zeros */
    uint16_t nh_be = htons((uint16_t)next_header);
    sum += nh_be;

    /* Sum data */
    p = (const uint16_t *)data;
    uint32_t rem = len;
    while (rem > 1) { sum += *p++; rem -= 2; }
    if (rem == 1) sum += *(const uint8_t *)p;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ── Build / send L2 frame ────────────────────── */

static int ipv6_send_with_src(struct ipv6_addr src, struct ipv6_addr dst,
                               uint8_t next_header,
                               const void *payload, uint16_t len)
{
    if (len + sizeof(struct ipv6_hdr) + sizeof(struct eth_hdr) > NET_FRAME_MAX)
        return -1;

    /* Choose destination MAC */
    struct mac_addr dst_mac;
    if (ipv6_is_multicast(dst)) {
        dst_mac = ipv6_multicast_mac(dst);
    } else {
        if (neigh_lookup(dst, &dst_mac) != 0) {
            /* Fire ND solicitation and wait briefly */
            extern int ipv6_neigh_resolve(struct ipv6_addr, struct mac_addr *);
            if (ipv6_neigh_resolve(dst, &dst_mac) != 0) return -1;
        }
    }

    uint8_t frame[NET_FRAME_MAX];
    struct eth_hdr *eth = (struct eth_hdr *)frame;
    eth->dst = dst_mac;
    eth->src = g_net.mac;
    eth->ethertype = htons(ETH_TYPE_IPV6);

    struct ipv6_hdr *ip6 = (struct ipv6_hdr *)(frame + sizeof(struct eth_hdr));
    /* Version=6, TC=0, flow=0 */
    ip6->ver_tc_fl   = htonl(0x60000000U);
    ip6->payload_len = htons(len);
    ip6->next_header = next_header;
    ip6->hop_limit   = (next_header == IPV6_NH_ICMPV6) ? 255 : 64;
    ip6->src         = src;
    ip6->dst         = dst;

    uint8_t *body = frame + sizeof(struct eth_hdr) + sizeof(struct ipv6_hdr);
    mc(body, payload, len);

    uint16_t total = sizeof(struct eth_hdr) + sizeof(struct ipv6_hdr) + len;
    return net_drv_send(frame, total);
}

int ipv6_send(struct ipv6_addr dst, uint8_t next_header,
              const void *payload, uint16_t len)
{
    struct ipv6_addr src = ipv6_pick_source(dst);
    return ipv6_send_with_src(src, dst, next_header, payload, len);
}

int ipv6_resolve_dst_mac(struct ipv6_addr dst, struct mac_addr *out)
{
    if (ipv6_is_multicast(dst)) { *out = ipv6_multicast_mac(dst); return 0; }
    return neigh_lookup(dst, out);
}

/* ── ICMPv6 send helpers ──────────────────────── */

static int icmpv6_send_from(struct ipv6_addr src, struct ipv6_addr dst,
                            uint8_t type, uint8_t code,
                            const void *body, uint16_t body_len)
{
    /* Build ICMPv6 in a buffer, compute checksum over pseudo-header + msg. */
    uint8_t buf[1280];
    if ((unsigned)body_len + 4 > sizeof(buf)) return -1;
    buf[0] = type; buf[1] = code; buf[2] = 0; buf[3] = 0;
    if (body && body_len) mc(buf + 4, body, body_len);
    uint16_t total = (uint16_t)(4 + body_len);

    uint16_t ck = ipv6_pseudo_checksum(&src, &dst, IPV6_NH_ICMPV6, buf, total);
    buf[2] = (uint8_t)(ck & 0xFF);
    buf[3] = (uint8_t)(ck >> 8);

    return ipv6_send_with_src(src, dst, IPV6_NH_ICMPV6, buf, total);
}

/* ── Neighbor Solicitation / Advertisement ────── */

static void send_neighbor_solicit(struct ipv6_addr target)
{
    /* Body: 4-byte reserved + target (16) + option (8: source LL addr) */
    uint8_t body[4 + 16 + 8];
    mz(body, sizeof(body));
    mc(body + 4, target.bytes, 16);
    body[20] = ND_OPT_SOURCE_LL_ADDR;
    body[21] = 1;  /* length in 8-byte units */
    mc(body + 22, g_net.mac.b, 6);

    struct ipv6_addr dst = ipv6_solicited_node(target);
    /* Use our link-local as source. */
    struct ipv6_addr src = {{0}};
    for (int i = 0; i < IPV6_ADDR_MAX; i++) {
        if (g_net6.addrs[i].valid && ipv6_is_link_local(g_net6.addrs[i].addr)) {
            src = g_net6.addrs[i].addr; break;
        }
    }
    icmpv6_send_from(src, dst, ICMPV6_NEIGHBOR_SOLICIT, 0, body, sizeof(body));
}

static void send_neighbor_advert(struct ipv6_addr target_addr,
                                  struct ipv6_addr requester,
                                  struct mac_addr requester_mac)
{
    (void)requester_mac;
    /* Flags: Solicited (0x40) + Override (0x20). */
    uint8_t body[4 + 16 + 8];
    mz(body, sizeof(body));
    body[0] = 0x60;
    mc(body + 4, target_addr.bytes, 16);
    body[20] = ND_OPT_TARGET_LL_ADDR;
    body[21] = 1;
    mc(body + 22, g_net.mac.b, 6);

    struct ipv6_addr dst = ipv6_is_unspecified(requester) ? ipv6_all_nodes() : requester;
    icmpv6_send_from(target_addr, dst, ICMPV6_NEIGHBOR_ADVERT, 0, body, sizeof(body));
}

static void send_router_solicit(void)
{
    uint8_t body[4 + 8];
    mz(body, sizeof(body));
    body[4] = ND_OPT_SOURCE_LL_ADDR;
    body[5] = 1;
    mc(body + 6, g_net.mac.b, 6);

    struct ipv6_addr src = {{0}};
    for (int i = 0; i < IPV6_ADDR_MAX; i++) {
        if (g_net6.addrs[i].valid && ipv6_is_link_local(g_net6.addrs[i].addr)) {
            src = g_net6.addrs[i].addr; break;
        }
    }
    icmpv6_send_from(src, ipv6_all_routers(), ICMPV6_ROUTER_SOLICIT, 0,
                     body, sizeof(body));
}

int ipv6_neigh_resolve(struct ipv6_addr target, struct mac_addr *out)
{
    if (neigh_lookup(target, out) == 0) return 0;

    s_neigh_resolved = 0;
    s_neigh_target = target;

    for (int attempt = 0; attempt < 3; attempt++) {
        send_neighbor_solicit(target);
        for (int i = 0; i < 5000; i++) {
            net_poll();
            if (neigh_lookup(target, out) == 0) return 0;
            for (volatile int j = 0; j < 5000; j++);
        }
    }
    return -1;
}

/* ── Process ICMPv6 ──────────────────────────── */

static void process_icmpv6(const struct ipv6_hdr *ip6,
                           const uint8_t *icmp, uint16_t len,
                           const struct mac_addr *src_mac)
{
    if (len < 4) return;
    uint8_t type = icmp[0];

    switch (type) {
    case ICMPV6_ECHO_REQUEST: {
        if (len < 8) return;
        /* Build echo reply — flip type, recompute checksum */
        uint8_t reply[1280];
        if (len > sizeof(reply)) return;
        mc(reply, icmp, len);
        reply[0] = ICMPV6_ECHO_REPLY;
        reply[2] = 0; reply[3] = 0;
        struct ipv6_addr our_src = ip6->dst;
        if (ipv6_is_multicast(our_src)) our_src = ipv6_pick_source(ip6->src);
        uint16_t ck = ipv6_pseudo_checksum(&our_src, &ip6->src,
                                            IPV6_NH_ICMPV6, reply, len);
        reply[2] = (uint8_t)(ck & 0xFF);
        reply[3] = (uint8_t)(ck >> 8);
        ipv6_send_with_src(our_src, ip6->src, IPV6_NH_ICMPV6, reply, len);
        break;
    }
    case ICMPV6_ECHO_REPLY:
        s_echo_recv_tsc = rdtsc6();
        s_echo_received = 1;
        break;

    case ICMPV6_NEIGHBOR_SOLICIT: {
        if (len < 24) return;
        struct ipv6_addr target;
        mc(target.bytes, icmp + 8, 16);
        /* Is the target one of ours? */
        int ours = 0;
        for (int i = 0; i < IPV6_ADDR_MAX; i++) {
            if (g_net6.addrs[i].valid && ipv6_eq(g_net6.addrs[i].addr, target)) {
                ours = 1; break;
            }
        }
        /* Cache requester's source LL addr if option present */
        if (len >= 32 && icmp[24] == ND_OPT_SOURCE_LL_ADDR) {
            struct mac_addr m;
            mc(m.b, icmp + 26, 6);
            if (!ipv6_is_unspecified(ip6->src)) neigh_add(ip6->src, m);
        } else if (src_mac) {
            if (!ipv6_is_unspecified(ip6->src)) neigh_add(ip6->src, *src_mac);
        }
        if (ours) {
            struct mac_addr rmac = src_mac ? *src_mac : (struct mac_addr){{0}};
            send_neighbor_advert(target, ip6->src, rmac);
        }
        break;
    }
    case ICMPV6_NEIGHBOR_ADVERT: {
        if (len < 24) return;
        struct ipv6_addr target;
        mc(target.bytes, icmp + 8, 16);
        struct mac_addr m;
        if (len >= 32 && icmp[24] == ND_OPT_TARGET_LL_ADDR) {
            mc(m.b, icmp + 26, 6);
        } else if (src_mac) {
            m = *src_mac;
        } else break;
        neigh_add(target, m);
        if (ipv6_eq(target, s_neigh_target)) s_neigh_resolved = 1;
        break;
    }
    case ICMPV6_ROUTER_ADVERT: {
        if (len < 16) return;
        /* Hop limit (1) + flags(1) + lifetime(2) + reachable(4) + retrans(4) */
        uint8_t flags = icmp[5];
        int m_bit = (flags & 0x80) != 0;
        /* Cache router LL */
        g_net6.gateway = ip6->src;
        g_net6.have_gateway = 1;
        if (src_mac) neigh_add(ip6->src, *src_mac);
        /* Walk options for prefix info / source LL addr */
        const uint8_t *opt = icmp + 16;
        const uint8_t *end = icmp + len;
        while (opt + 8 <= end) {
            uint8_t otype = opt[0];
            uint8_t olen  = opt[1];
            if (olen == 0) break;
            int oblen = olen * 8;
            if (opt + oblen > end) break;
            if (otype == ND_OPT_SOURCE_LL_ADDR && oblen >= 8) {
                struct mac_addr m; mc(m.b, opt + 2, 6);
                neigh_add(ip6->src, m);
            } else if (otype == ND_OPT_PREFIX_INFO && oblen >= 32) {
                uint8_t prefix_len = opt[2];
                uint8_t pflags     = opt[3];
                int autoconf = (pflags & 0x40) != 0;
                if (autoconf && prefix_len == 64) {
                    /* Form SLAAC address: prefix(64) || EUI-64(64) */
                    struct ipv6_addr a;
                    mc(a.bytes, opt + 16, 8);
                    /* EUI-64 from MAC */
                    a.bytes[8]  = g_net.mac.b[0] ^ 0x02;
                    a.bytes[9]  = g_net.mac.b[1];
                    a.bytes[10] = g_net.mac.b[2];
                    a.bytes[11] = 0xFF;
                    a.bytes[12] = 0xFE;
                    a.bytes[13] = g_net.mac.b[3];
                    a.bytes[14] = g_net.mac.b[4];
                    a.bytes[15] = g_net.mac.b[5];
                    /* Add if new */
                    int have = 0;
                    for (int i = 0; i < IPV6_ADDR_MAX; i++) {
                        if (g_net6.addrs[i].valid && ipv6_eq(g_net6.addrs[i].addr, a)) {
                            have = 1; break;
                        }
                    }
                    if (!have) {
                        for (int i = 0; i < IPV6_ADDR_MAX; i++) {
                            if (!g_net6.addrs[i].valid) {
                                g_net6.addrs[i].addr = a;
                                g_net6.addrs[i].prefix_len = 64;
                                g_net6.addrs[i].is_dhcp = 0;
                                g_net6.addrs[i].valid = 1;
                                break;
                            }
                        }
                    }
                }
            }
            opt += oblen;
        }
        if (m_bit) g_net6.dhcpv6_active = 1;
        break;
    }
    default:
        break;
    }
}

/* ── Main RX entry from net_poll ─────────────── */

void ipv6_process(const void *frame, uint16_t len)
{
    if (len < sizeof(struct eth_hdr) + sizeof(struct ipv6_hdr)) return;

    const struct eth_hdr  *eth = (const struct eth_hdr *)frame;
    const struct ipv6_hdr *ip6 = (const struct ipv6_hdr *)((const uint8_t *)frame + sizeof(struct eth_hdr));

    /* Version check */
    if ((ntohl(ip6->ver_tc_fl) >> 28) != 6) return;

    uint16_t plen = ntohs(ip6->payload_len);
    if (plen + sizeof(struct ipv6_hdr) + sizeof(struct eth_hdr) > len) return;

    /* Accept if dst is ours, multicast, or all-nodes/sol-node */
    int for_us = 0;
    if (ipv6_is_multicast(ip6->dst)) for_us = 1;
    for (int i = 0; i < IPV6_ADDR_MAX && !for_us; i++) {
        if (g_net6.addrs[i].valid && ipv6_eq(g_net6.addrs[i].addr, ip6->dst))
            for_us = 1;
    }
    if (!for_us) return;

    const uint8_t *payload = (const uint8_t *)ip6 + sizeof(struct ipv6_hdr);
    uint8_t nh = ip6->next_header;

    /* Minimal hop-by-hop skip */
    while (nh == IPV6_NH_HOPOPT) {
        if (plen < 8) return;
        uint8_t next = payload[0];
        uint8_t hlen8 = (uint8_t)((payload[1] + 1) * 8);
        if (hlen8 > plen) return;
        nh = next;
        payload += hlen8;
        plen   -= hlen8;
    }

    switch (nh) {
    case IPV6_NH_ICMPV6:
        process_icmpv6(ip6, payload, plen, &eth->src);
        break;
    case IPV6_NH_UDP:
        udp6_process(&ip6->src, &ip6->dst, payload, plen);
        break;
    case IPV6_NH_TCP:
        tcp6_process(&ip6->src, &ip6->dst, payload, plen);
        break;
    default:
        break;
    }
}

/* ── ping6 ─────────────────────────────────── */

uint64_t ipv6_ping(struct ipv6_addr target)
{
    uint8_t body[60];
    /* identifier + sequence + 56 bytes of pattern */
    body[0] = 0x12; body[1] = 0x34;
    body[2] = (uint8_t)(s_echo_id_seq >> 8);
    body[3] = (uint8_t)(s_echo_id_seq & 0xFF);
    s_echo_id_seq++;
    for (int i = 4; i < 60; i++) body[i] = (uint8_t)i;

    struct ipv6_addr src = ipv6_pick_source(target);

    /* Build ICMPv6 message: type+code+ck (4) + body (60) */
    uint8_t msg[64];
    msg[0] = ICMPV6_ECHO_REQUEST;
    msg[1] = 0; msg[2] = 0; msg[3] = 0;
    mc(msg + 4, body, 60);
    uint16_t ck = ipv6_pseudo_checksum(&src, &target, IPV6_NH_ICMPV6, msg, 64);
    msg[2] = (uint8_t)(ck & 0xFF);
    msg[3] = (uint8_t)(ck >> 8);

    s_echo_received = 0;
    s_echo_send_tsc = rdtsc6();

    if (ipv6_send_with_src(src, target, IPV6_NH_ICMPV6, msg, 64) < 0)
        return 0;

    for (int i = 0; i < 30000 && !s_echo_received; i++) {
        net_poll();
        for (volatile int j = 0; j < 10000; j++);
    }
    if (s_echo_received) return s_echo_recv_tsc - s_echo_send_tsc;
    return 0;
}

/* ── Init: build link-local, send RS, optional DHCPv6 ── */

extern int dhcpv6_run(void);  /* in net_dhcpv6.c */

/* RS retry pacing for the async path (ipv6_service). */
static uint64_t s_last_rs_tsc = 0;
static int      s_rs_count = 0;

/* True once SLAAC (or DHCPv6) has given us a non-link-local address. */
int ipv6_have_global(void)
{
    for (int i = 0; i < IPV6_ADDR_MAX; i++)
        if (g_net6.addrs[i].valid && !ipv6_is_link_local(g_net6.addrs[i].addr))
            return 1;
    return 0;
}

/* Non-blocking bring-up: build the link-local address and fire the first Router
 * Solicitation. SLAAC completes ASYNCHRONOUSLY when the RA arrives via the RX
 * path (ipv6_process, pumped by net_service each tick) — no boot-blocking wait,
 * matching how DHCP was moved async. Safe to call once from net_init(). */
int ipv6_start(void)
{
    if (s_initialised) return 0;

    for (int i = 0; i < IPV6_ADDR_MAX; i++) g_net6.addrs[i].valid = 0;
    for (int i = 0; i < IPV6_NEIGH_CACHE_SIZE; i++) g_net6.neigh[i].valid = 0;
    g_net6.have_gateway = 0;
    g_net6.dhcpv6_active = 0;

    /* Build link-local fe80::/64 || EUI-64 */
    struct ipv6_addr ll;
    mz(ll.bytes, 16);
    ll.bytes[0] = 0xFE; ll.bytes[1] = 0x80;
    ll.bytes[8]  = g_net.mac.b[0] ^ 0x02;
    ll.bytes[9]  = g_net.mac.b[1];
    ll.bytes[10] = g_net.mac.b[2];
    ll.bytes[11] = 0xFF;
    ll.bytes[12] = 0xFE;
    ll.bytes[13] = g_net.mac.b[3];
    ll.bytes[14] = g_net.mac.b[4];
    ll.bytes[15] = g_net.mac.b[5];
    g_net6.addrs[0].addr = ll;
    g_net6.addrs[0].prefix_len = 64;
    g_net6.addrs[0].is_dhcp = 0;
    g_net6.addrs[0].valid = 1;

    s_initialised = 1;
    send_router_solicit();
    s_last_rs_tsc = rdtsc6();
    s_rs_count = 1;

    char buf[48];
    ipv6_format(ll, buf);
    kputs("IPv6: link-local ");
    kputs(buf);
    kputs(" (RS sent, SLAAC async)\n");
    return 0;
}

/* Per-tick service (call from net_service): retry the Router Solicitation until
 * SLAAC lands a global address, then run DHCPv6 if the RA set the M-bit. Bounded
 * retries so a v6-less network costs a few packets, not a spin. */
void ipv6_service(void)
{
    if (!s_initialised || ipv6_have_global()) return;
    if (s_rs_count >= 8) return;                 /* give up after 8 solicitations */
    uint64_t now = rdtsc6();
    if (now - s_last_rs_tsc < 2000000000ULL) return;  /* ~1s between RS */
    send_router_solicit();
    s_last_rs_tsc = now;
    s_rs_count++;
    if (g_net6.dhcpv6_active) dhcpv6_run();       /* managed flag → DHCPv6 */
}

/* Blocking bring-up (link-local + RS + a short RA-absorb window). Retained for
 * standalone/selftest callers; the boot path uses ipv6_start()+ipv6_service(). */
int ipv6_init(void)
{
    if (s_initialised) return 0;
    ipv6_start();
    for (int i = 0; i < 2000; i++) {
        net_poll();
        for (volatile int j = 0; j < 5000; j++);
    }
    if (g_net6.dhcpv6_active) dhcpv6_run();
    return 0;
}

/* ── Selftest ────────────────────────────────── */

void ipv6_print_selftest_line(void)
{
    char buf[48];
    struct ipv6_addr ll = {{0}};
    int have_dhcp = 0, have_aaaa = 0;
    for (int i = 0; i < IPV6_ADDR_MAX; i++) {
        if (!g_net6.addrs[i].valid) continue;
        if (ipv6_is_link_local(g_net6.addrs[i].addr)) ll = g_net6.addrs[i].addr;
        if (g_net6.addrs[i].is_dhcp) have_dhcp = 1;
    }
    /* Try a quick AAAA lookup against our DNS to see if the resolver works */
    extern int dns_resolve_aaaa(const char *, struct ipv6_addr *);
    struct ipv6_addr probe;
    if (dns_resolve_aaaa("ipv6.google.com", &probe) == 0) have_aaaa = 1;

    ipv6_format(ll, buf);
    kputs("  IPv6 .................. link-local ");
    kputs(buf);
    kputs(", DHCPv6=");
    kputs(have_dhcp ? "Y" : "N");
    kputs(", AAAA=");
    kputs(have_aaaa ? "working" : "no");
    kputs("\n");
}
