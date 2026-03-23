/*
 * Zeos — ARP implementation
 */

#include "net_arp.h"
#include "net_virtio.h"
#include "kprint.h"
#include "timer.h"

/* ARP cache */
#define ARP_CACHE_SIZE 16
static struct {
    struct ipv4_addr ip;
    struct mac_addr  mac;
    int              valid;
} arp_cache[ARP_CACHE_SIZE];

static const struct mac_addr mac_broadcast = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
static const struct mac_addr mac_zero = {{0, 0, 0, 0, 0, 0}};

static void arp_cache_add(struct ipv4_addr ip, struct mac_addr mac)
{
    /* Check if already cached */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && ip_eq(arp_cache[i].ip, ip)) {
            arp_cache[i].mac = mac;
            return;
        }
    }
    /* Find empty slot */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip;
            arp_cache[i].mac = mac;
            arp_cache[i].valid = 1;
            return;
        }
    }
    /* Overwrite slot 0 */
    arp_cache[0].ip = ip;
    arp_cache[0].mac = mac;
    arp_cache[0].valid = 1;
}

static int arp_cache_lookup(struct ipv4_addr ip, struct mac_addr *out)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && ip_eq(arp_cache[i].ip, ip)) {
            *out = arp_cache[i].mac;
            return 0;
        }
    }
    return -1;
}

void arp_process(const void *frame, uint16_t len)
{
    if (len < sizeof(struct eth_hdr) + sizeof(struct arp_hdr))
        return;

    const struct arp_hdr *arp = (const struct arp_hdr *)((const uint8_t *)frame + sizeof(struct eth_hdr));

    uint16_t op = ntohs(arp->operation);

    /* Cache the sender regardless of operation */
    arp_cache_add(arp->sender_ip, arp->sender_mac);

    if (op == 1 && ip_eq(arp->target_ip, g_net.ip)) {
        /* ARP request for us — send reply */
        uint8_t reply[sizeof(struct eth_hdr) + sizeof(struct arp_hdr)];
        struct eth_hdr *eth = (struct eth_hdr *)reply;
        struct arp_hdr *rarp = (struct arp_hdr *)(reply + sizeof(struct eth_hdr));

        eth->dst = arp->sender_mac;
        eth->src = g_net.mac;
        eth->ethertype = htons(ETH_TYPE_ARP);

        rarp->hw_type = htons(1);
        rarp->proto_type = htons(ETH_TYPE_IP);
        rarp->hw_len = 6;
        rarp->proto_len = 4;
        rarp->operation = htons(2);  /* Reply */
        rarp->sender_mac = g_net.mac;
        rarp->sender_ip = g_net.ip;
        rarp->target_mac = arp->sender_mac;
        rarp->target_ip = arp->sender_ip;

        virtio_net_send(reply, sizeof(reply));
    }
}

static void arp_send_request(struct ipv4_addr target)
{
    uint8_t pkt[sizeof(struct eth_hdr) + sizeof(struct arp_hdr)];
    struct eth_hdr *eth = (struct eth_hdr *)pkt;
    struct arp_hdr *arp = (struct arp_hdr *)(pkt + sizeof(struct eth_hdr));

    eth->dst = mac_broadcast;
    eth->src = g_net.mac;
    eth->ethertype = htons(ETH_TYPE_ARP);

    arp->hw_type = htons(1);
    arp->proto_type = htons(ETH_TYPE_IP);
    arp->hw_len = 6;
    arp->proto_len = 4;
    arp->operation = htons(1);  /* Request */
    arp->sender_mac = g_net.mac;
    arp->sender_ip = g_net.ip;
    arp->target_mac = mac_zero;
    arp->target_ip = target;

    virtio_net_send(pkt, sizeof(pkt));
}

int arp_resolve(struct ipv4_addr target, struct mac_addr *out)
{
    /* Check cache first */
    if (arp_cache_lookup(target, out) == 0)
        return 0;

    /* Send request and wait for reply */
    for (int attempt = 0; attempt < 5; attempt++) {
        arp_send_request(target);

        /* Poll for up to 1 second */
        for (int i = 0; i < 10000; i++) {
            net_poll();
            if (arp_cache_lookup(target, out) == 0)
                return 0;
            /* Small busy-wait */
            for (volatile int j = 0; j < 10000; j++);
        }
    }

    return -1;  /* Timeout */
}
