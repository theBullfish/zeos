/*
 * Zeos — IPv4 + ICMP implementation
 */

#include "net_ip.h"
#include "net_arp.h"
#include "net_virtio.h"
#include "kprint.h"

/* Forward declarations for protocol handlers */
extern void tcp_process(const void *ip_pkt, uint16_t ip_len);
extern void udp_process(const void *ip_pkt, uint16_t ip_len);

static uint16_t ip_id_counter = 1;

/* ICMP state for ping */
static volatile int icmp_reply_received = 0;
static uint64_t icmp_send_tsc = 0;
static uint64_t icmp_recv_tsc = 0;

static uint64_t read_tsc_net(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

int ip_send(struct ipv4_addr dst, uint8_t protocol, const void *payload, uint16_t len)
{
    /* Determine next-hop: gateway if off-subnet, direct if on-subnet */
    struct ipv4_addr next_hop;
    uint32_t dst_u = ip_to_u32(dst);
    uint32_t our_u = ip_to_u32(g_net.ip);
    uint32_t mask_u = ip_to_u32(g_net.netmask);

    if ((dst_u & mask_u) == (our_u & mask_u))
        next_hop = dst;
    else
        next_hop = g_net.gateway;

    /* ARP resolve */
    struct mac_addr dst_mac;
    if (arp_resolve(next_hop, &dst_mac) < 0)
        return -1;

    /* Build frame */
    uint16_t total = sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr) + len;
    if (total > NET_FRAME_MAX) return -1;

    uint8_t frame[NET_FRAME_MAX];

    /* Ethernet header */
    struct eth_hdr *eth = (struct eth_hdr *)frame;
    eth->dst = dst_mac;
    eth->src = g_net.mac;
    eth->ethertype = htons(ETH_TYPE_IP);

    /* IPv4 header */
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(frame + sizeof(struct eth_hdr));
    ip->ver_ihl = 0x45;    /* IPv4, 5 dwords (20 bytes) */
    ip->tos = 0;
    ip->total_len = htons(sizeof(struct ipv4_hdr) + len);
    ip->id = htons(ip_id_counter++);
    ip->flags_frag = htons(0x4000);  /* Don't fragment */
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->checksum = 0;
    ip->src = g_net.ip;
    ip->dst = dst;

    /* Compute IP header checksum */
    ip->checksum = net_checksum(ip, sizeof(struct ipv4_hdr));

    /* Copy payload */
    uint8_t *payload_dst = frame + sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr);
    const uint8_t *payload_src = (const uint8_t *)payload;
    for (uint16_t i = 0; i < len; i++)
        payload_dst[i] = payload_src[i];

    return virtio_net_send(frame, total);
}

void ip_process(const void *frame, uint16_t len)
{
    if (len < sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr))
        return;

    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)((const uint8_t *)frame + sizeof(struct eth_hdr));

    /* Verify it's for us (or broadcast) */
    if (!ip_eq(ip->dst, g_net.ip)) {
        /* Check broadcast */
        struct ipv4_addr bcast = {{255, 255, 255, 255}};
        if (!ip_eq(ip->dst, bcast))
            return;
    }

    uint16_t ip_hdr_len = (ip->ver_ihl & 0x0F) * 4;
    uint16_t ip_total = ntohs(ip->total_len);
    const uint8_t *payload = (const uint8_t *)ip + ip_hdr_len;
    uint16_t payload_len = ip_total - ip_hdr_len;

    switch (ip->protocol) {
    case IP_PROTO_ICMP:
        /* Handle ICMP echo reply */
        if (payload_len >= 8) {
            uint8_t icmp_type = payload[0];
            if (icmp_type == 0) {  /* Echo reply */
                icmp_recv_tsc = read_tsc_net();
                icmp_reply_received = 1;
            } else if (icmp_type == 8) {  /* Echo request — reply */
                /* Build echo reply */
                uint8_t reply_payload[128];
                uint16_t rlen = payload_len > 128 ? 128 : payload_len;
                for (uint16_t i = 0; i < rlen; i++)
                    reply_payload[i] = payload[i];
                reply_payload[0] = 0;   /* Type: echo reply */
                reply_payload[2] = 0;   /* Clear checksum */
                reply_payload[3] = 0;
                /* Recompute checksum */
                uint16_t ck = net_checksum(reply_payload, rlen);
                reply_payload[2] = ck & 0xFF;
                reply_payload[3] = (ck >> 8) & 0xFF;
                ip_send(ip->src, IP_PROTO_ICMP, reply_payload, rlen);
            }
        }
        break;

    case IP_PROTO_TCP:
        tcp_process(frame, len);
        break;

    case IP_PROTO_UDP:
        udp_process(frame, len);
        break;
    }
}

uint64_t icmp_ping(struct ipv4_addr target)
{
    /* Build ICMP echo request */
    uint8_t icmp[64];
    icmp[0] = 8;    /* Type: echo request */
    icmp[1] = 0;    /* Code */
    icmp[2] = 0;    /* Checksum (computed below) */
    icmp[3] = 0;
    icmp[4] = 0;    /* Identifier */
    icmp[5] = 1;
    icmp[6] = 0;    /* Sequence */
    icmp[7] = 1;
    /* Payload: timestamp */
    for (int i = 8; i < 64; i++)
        icmp[i] = (uint8_t)i;

    uint16_t ck = net_checksum(icmp, 64);
    icmp[2] = ck & 0xFF;
    icmp[3] = (ck >> 8) & 0xFF;

    icmp_reply_received = 0;
    icmp_send_tsc = read_tsc_net();

    if (ip_send(target, IP_PROTO_ICMP, icmp, 64) < 0)
        return 0;

    /* Wait for reply (up to 3 seconds) */
    for (int i = 0; i < 30000 && !icmp_reply_received; i++) {
        net_poll();
        for (volatile int j = 0; j < 10000; j++);
    }

    if (icmp_reply_received)
        return icmp_recv_tsc - icmp_send_tsc;

    return 0;  /* Timeout */
}
