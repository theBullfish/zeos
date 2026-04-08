/*
 * Zeos — Network Stack Init & Poll
 *
 * Glues all layers together. Dispatches incoming packets
 * by ethertype. Provides the net_poll() function that
 * every blocking network call uses internally.
 */

#include "net.h"
#include "net_virtio.h"
#include "net_arp.h"
#include "net_ip.h"
#include "net_tcp.h"
#include "net_dhcp.h"
#include "kprint.h"

/* Global network config */
struct net_config g_net;

int net_init(void)
{
    /* Fallback addresses (QEMU SLIRP defaults) */
    g_net.ip      = (struct ipv4_addr){{10, 0, 2, 15}};
    g_net.gateway = (struct ipv4_addr){{10, 0, 2, 2}};
    g_net.netmask = (struct ipv4_addr){{255, 255, 255, 0}};
    g_net.dns     = (struct ipv4_addr){{10, 0, 2, 3}};
    g_net.up = 0;

    /* Initialize virtio-net driver */
    if (virtio_net_init() < 0) {
        kputs("NET: no network device found\n");
        return -1;
    }

    /* Get our MAC */
    virtio_net_get_mac(&g_net.mac);

    g_net.up = 1;

    /* Try DHCP — if it fails, fallback addresses stay in place */
    if (dhcp_discover() < 0) {
        kputs("NET: DHCP failed, using fallback config\n");
    }

    kputs("NET: ");
    for (int i = 0; i < 6; i++) {
        if (i > 0) kputs(":");
        uint8_t b = g_net.mac.b[i];
        static const char hex[] = "0123456789abcdef";
        kputc(hex[(b >> 4) & 0xf]);
        kputc(hex[b & 0xf]);
    }
    kputs(" @ ");
    kput_dec(g_net.ip.b[0]); kputs(".");
    kput_dec(g_net.ip.b[1]); kputs(".");
    kput_dec(g_net.ip.b[2]); kputs(".");
    kput_dec(g_net.ip.b[3]);
    kputs("\n");

    return 0;
}

void net_poll(void)
{
    if (!g_net.up) return;

    uint8_t frame[NET_BUF_SIZE];
    int len = virtio_net_recv(frame, NET_BUF_SIZE);

    if (len < (int)sizeof(struct eth_hdr))
        return;

    struct eth_hdr *eth = (struct eth_hdr *)frame;
    uint16_t ethertype = ntohs(eth->ethertype);

    switch (ethertype) {
    case ETH_TYPE_ARP:
        arp_process(frame, (uint16_t)len);
        break;
    case ETH_TYPE_IP:
        ip_process(frame, (uint16_t)len);
        break;
    }

    /* Check retransmission timers after processing any packet */
    tcp_retransmit_tick();
}

void net_poll_wait(uint32_t timeout_ms)
{
    /* Approximate: poll in a loop.
     * TODO: use timer for accurate timing */
    for (uint32_t i = 0; i < timeout_ms * 100; i++) {
        net_poll();
        for (volatile int j = 0; j < 100; j++);
    }
}
