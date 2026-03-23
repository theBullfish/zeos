/*
 * Zeos — UDP implementation
 */

#include "net_udp.h"
#include "net_ip.h"
#include "kprint.h"

/* Port binding table */
#define UDP_MAX_BINDS 8
static struct {
    uint16_t    port;
    udp_recv_fn callback;
} udp_binds[UDP_MAX_BINDS];

int udp_bind(uint16_t port, udp_recv_fn callback)
{
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (udp_binds[i].port == 0) {
            udp_binds[i].port = port;
            udp_binds[i].callback = callback;
            return 0;
        }
    }
    return -1;
}

int udp_send(struct ipv4_addr dst, uint16_t src_port, uint16_t dst_port,
             const void *data, uint16_t len)
{
    uint8_t pkt[NET_MTU];
    struct udp_hdr *udp = (struct udp_hdr *)pkt;

    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons(sizeof(struct udp_hdr) + len);
    udp->checksum = 0;  /* Optional in IPv4 */

    const uint8_t *src = (const uint8_t *)data;
    for (uint16_t i = 0; i < len; i++)
        pkt[sizeof(struct udp_hdr) + i] = src[i];

    return ip_send(dst, IP_PROTO_UDP, pkt, sizeof(struct udp_hdr) + len);
}

void udp_process(const void *frame, uint16_t len)
{
    const struct eth_hdr *eth = (const struct eth_hdr *)frame;
    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)((const uint8_t *)frame + sizeof(struct eth_hdr));
    uint16_t ip_hdr_len = (ip->ver_ihl & 0x0F) * 4;
    const struct udp_hdr *udp = (const struct udp_hdr *)((const uint8_t *)ip + ip_hdr_len);

    (void)eth;
    (void)len;

    uint16_t dst_port = ntohs(udp->dst_port);
    uint16_t src_port = ntohs(udp->src_port);
    uint16_t udp_len = ntohs(udp->length);
    uint16_t data_len = udp_len - sizeof(struct udp_hdr);
    const uint8_t *data = (const uint8_t *)udp + sizeof(struct udp_hdr);

    /* Dispatch to bound port */
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (udp_binds[i].port == dst_port && udp_binds[i].callback) {
            udp_binds[i].callback(ip->src, src_port, data, data_len);
            return;
        }
    }
}
