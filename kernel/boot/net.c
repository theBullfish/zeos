/*
 * Zeos — Network Stack Init & Poll
 *
 * Glues all layers together. Dispatches incoming packets
 * by ethertype. Provides the net_poll() function that
 * every blocking network call uses internally.
 */

#include "net.h"
#include "net_virtio.h"
#include "net_e1000.h"
#include "net_rtl8139.h"
#include "net_arp.h"
#include "net_ip.h"
#include "net_tcp.h"
#include "net_dhcp.h"
#include "kprint.h"

/* Global network config */
struct net_config g_net;

/* Driver dispatch function pointers */
int  (*net_drv_send)(const void *data, uint16_t len);
int  (*net_drv_recv)(void *buf, uint16_t max_len);
void (*net_drv_get_mac)(struct mac_addr *mac);

int net_init(void)
{
    /* Start with everything zero. We try DHCP after driver init; if DHCP
     * fails we leave the addresses zero and any DNS / TCP / TLS attempt
     * will surface a clean failure rather than send packets to QEMU's
     * default subnet from a real-hardware boot. */
    g_net.ip      = (struct ipv4_addr){{0, 0, 0, 0}};
    g_net.gateway = (struct ipv4_addr){{0, 0, 0, 0}};
    g_net.netmask = (struct ipv4_addr){{0, 0, 0, 0}};
    g_net.dns     = (struct ipv4_addr){{0, 0, 0, 0}};
    g_net.up = 0;

    /* Driver probe order: virtio-net (QEMU/cloud), e1000/e1000e (Intel
     * laptops/desktops 2005+), rtl8139 (cheap consumer 2002-2010).
     * First success wins. */
    if (virtio_net_init() == 0) {
        net_drv_send    = virtio_net_send;
        net_drv_recv    = virtio_net_recv;
        net_drv_get_mac = virtio_net_get_mac;
        kputs("NET: using virtio-net driver\n");
    } else if (e1000_init() == 0) {
        net_drv_send    = e1000_send;
        net_drv_recv    = e1000_recv;
        net_drv_get_mac = e1000_get_mac;
        kputs("NET: using e1000 driver\n");
    } else if (rtl8139_init() == 0) {
        net_drv_send    = rtl8139_send;
        net_drv_recv    = rtl8139_recv;
        net_drv_get_mac = rtl8139_get_mac;
        kputs("NET: using rtl8139 driver\n");
    } else {
        kputs("NET: no network device found\n");
        return -1;
    }

    /* Get our MAC */
    net_drv_get_mac(&g_net.mac);

    g_net.up = 1;

    /* Try DHCP. If it fails, downstream calls (DNS, TCP, TLS) will
     * fail cleanly because addresses are zero. The 'static-ip' shell
     * command can be used to set things by hand for diagnostic
     * sessions on networks without DHCP. */
    if (dhcp_discover() < 0) {
        kputs("NET: DHCP failed — no IP. Use 'static-ip' to configure manually.\n");
        g_net.up = 0;  /* mark down so https_get/etc skip cleanly */
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

    /* Bring up TLS subsystem (mbedTLS init + PSA + DRBG seed + CA load).
     * On failure HTTPS is unavailable; HTTP and the rest of the kernel
     * keep working. */
    extern int tls_init(void);
    tls_init();

    return 0;
}

void net_poll(void)
{
    if (!g_net.up) return;

    uint8_t frame[NET_BUF_SIZE];
    int len = net_drv_recv(frame, NET_BUF_SIZE);

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
