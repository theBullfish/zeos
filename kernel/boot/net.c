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
#include "net_rtl8169.h"
#include "net_rtl8139.h"
#include "usb_eth.h"
#include "net_arp.h"
#include "net_ip.h"
#include "net_ipv6.h"
#include "net_tcp.h"
#include "net_tcp6.h"
#include "net_dhcp.h"
#include "net_chain.h"
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
     * First success wins. The chosen driver is registered as the
     * hardware_dma backend for the net_tx / net_rx chains; the legacy
     * net_drv_send / net_drv_recv pointers are then redirected through
     * the chain so every TX and every RX flows through the typed
     * pipeline. */
    struct net_hw_ops hw = {0, 0, 0};
    if (virtio_net_init() == 0) {
        hw.tx = virtio_net_send; hw.rx_poll = virtio_net_recv; hw.name = "virtio-net";
        net_drv_get_mac = virtio_net_get_mac;
        kputs("NET: using virtio-net driver\n");
    } else if (e1000_init() == 0) {
        hw.tx = e1000_send; hw.rx_poll = e1000_recv; hw.name = "e1000";
        net_drv_get_mac = e1000_get_mac;
        kputs("NET: using e1000 driver\n");
    } else if (rtl8169_init() == 0) {
        hw.tx = rtl8169_send; hw.rx_poll = rtl8169_recv; hw.name = "rtl8169";
        net_drv_get_mac = rtl8169_get_mac;
        kputs("NET: using rtl8169 driver\n");
    } else if (rtl8139_init() == 0) {
        hw.tx = rtl8139_send; hw.rx_poll = rtl8139_recv; hw.name = "rtl8139";
        net_drv_get_mac = rtl8139_get_mac;
        kputs("NET: using rtl8139 driver\n");
    } else if (usb_eth_init() == 0) {
        hw.tx = usb_eth_send; hw.rx_poll = usb_eth_recv; hw.name = "usb-eth";
        net_drv_get_mac = usb_eth_get_mac;
        kputs("NET: using usb_eth driver\n");
    } else {
        kputs("NET: no network device found\n");
        return -1;
    }

    /* Wire the hw backend into the chain layer, then point the legacy
     * dispatch pointers at the chain shims so existing callers (ARP,
     * IP, DHCP, TCP, TLS, DNS) traverse the chain without any code
     * changes. The chains themselves get *registered* later by
     * chain_registry_init(); during the brief window before that the
     * shim falls back to direct hw calls (DHCP discover happens in
     * that window). */
    net_chain_set_hw(&hw);
    net_drv_send = net_chain_send;
    net_drv_recv = net_chain_recv;

    /* Get our MAC */
    net_drv_get_mac(&g_net.mac);

    g_net.up = 1;

    /* Kick DHCP off ASYNCHRONOUSLY (non-blocking). The blocking busy-loop used
     * to run here, before scheduler_run() — but network RX is pumped BY the
     * scheduler (net_service() each tick), so blocking here could never receive
     * a reply and stalled boot. dhcp_start() just sends DISCOVER and returns;
     * dhcp_service() (per tick) drives it to a bound lease. */
    dhcp_start();

    /* Bring IPv6 up async too: link-local + Router Solicitation now, SLAAC
     * completes over the next few net_service() ticks (ipv6_service). This is
     * the async-under-the-scheduler treatment the old blocking ipv6_init needed. */
    { extern int ipv6_start(void); ipv6_start(); }

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

    /* NOTE: ipv6_init() (Router Solicitation + SLAAC) and tls_init() (mbedTLS
     * CA/DRBG) also block on the boot path — ipv6_init busy-waits on an RA the
     * same way DHCP did, so it stalls boot before the scheduler exists. Deferred
     * out of net_init for now; they need the same async-under-the-scheduler
     * treatment as DHCP (net_late_init driven from net_service once bound).
     * IPv4 (DHCP + UDP/TCP + NTP) works without them. */
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
    case ETH_TYPE_IPV6:
        ipv6_process(frame, (uint16_t)len);
        break;
    }

    /* Check retransmission timers after processing any packet */
    tcp_retransmit_tick();
    tcp6_retransmit_tick();

    /* Drive the std.http accept-loop drain.  Weak-linked: if std_http
     * isn't compiled into this build, the symbol resolves to 0 and the
     * call is skipped.  Skip until at least one listener is armed so
     * the early DHCP/ARP path stays a tight busy-poll. */
    extern int  tcp_listener_count(void) __attribute__((weak));
    extern void zp_http_poll(void)       __attribute__((weak));
    extern void tls_srv_poll(void)       __attribute__((weak));
    if (&tcp_listener_count && &zp_http_poll &&
        tcp_listener_count() > 0)
        zp_http_poll();
    if (&tcp_listener_count && &tls_srv_poll &&
        tcp_listener_count() > 0)
        tls_srv_poll();
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

/* Per-tick network service, called from the scheduler loop: receive+dispatch
 * one batch of frames, then advance the async DHCP state machine. This is how
 * network I/O gets pumped BY the scheduler instead of blocking before it. */
void net_service(void)
{
    extern void dhcp_service(void);
    if (!g_net.up) return;
    net_poll();
    net_poll();      /* drain a couple frames per tick */
    net_poll();
    dhcp_service();
    { extern void ipv6_service(void); ipv6_service(); }   /* SLAAC retry until global addr */
}
