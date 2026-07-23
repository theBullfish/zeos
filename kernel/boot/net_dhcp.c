/*
 * Zeos — DHCP Client
 *
 * Four-packet dance: Discover → Offer → Request → Ack
 * All UDP, broadcast on 255.255.255.255:67, listen on :68.
 *
 * This is bare-metal. No malloc, no threads, no timers.
 * Uses the existing UDP layer and net_poll_wait().
 */

#include "net_dhcp.h"
#include "net_udp.h"
#include "net_ip.h"
#include "net.h"
#include "kprint.h"

/* ── State ── */
static struct dhcp_lease g_lease;
static volatile int      dhcp_got_offer = 0;
static volatile int      dhcp_got_ack   = 0;
static struct dhcp_msg   dhcp_reply;

/* ── Helpers ── */

static void mem_zero(void *p, int len) {
    uint8_t *b = (uint8_t *)p;
    for (int i = 0; i < len; i++) b[i] = 0;
}

static void mem_copy(void *dst, const void *src, int len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (int i = 0; i < len; i++) d[i] = s[i];
}

/* Generate a pseudo-random transaction ID from TSC */
static uint32_t dhcp_xid(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return lo ^ (hi << 16) ^ 0x5A454F53;  /* 'ZEOS' */
}

/* Find a DHCP option in the options field. Returns pointer to value, sets *out_len. */
static const uint8_t *dhcp_find_option(const uint8_t *opts, int opts_len,
                                        uint8_t code, int *out_len)
{
    int i = 0;
    while (i < opts_len) {
        uint8_t opt = opts[i];
        if (opt == DHCP_OPT_PAD) { i++; continue; }
        if (opt == DHCP_OPT_END) break;
        if (i + 1 >= opts_len) break;
        uint8_t len = opts[i + 1];
        if (opt == code) {
            *out_len = len;
            return &opts[i + 2];
        }
        i += 2 + len;
    }
    return 0;
}

/* Get DHCP message type from options */
static int dhcp_msg_type(const struct dhcp_msg *msg) {
    int len = 0;
    const uint8_t *val = dhcp_find_option(msg->options, 312, DHCP_OPT_MSG_TYPE, &len);
    if (val && len >= 1) return val[0];
    return -1;
}

/* ── Build messages ── */

static uint32_t g_xid;

static int dhcp_build_discover(uint8_t *buf) {
    struct dhcp_msg *msg = (struct dhcp_msg *)buf;
    mem_zero(msg, sizeof(*msg));

    msg->op    = 1;              /* BOOTREQUEST */
    msg->htype = 1;              /* Ethernet */
    msg->hlen  = 6;
    msg->xid   = htonl(g_xid);
    msg->flags = htons(0x8000);  /* Broadcast flag */
    msg->magic = htonl(DHCP_MAGIC);

    /* Copy our MAC */
    mem_copy(msg->chaddr, g_net.mac.b, 6);

    /* Options */
    int i = 0;
    /* Message type = DISCOVER */
    msg->options[i++] = DHCP_OPT_MSG_TYPE;
    msg->options[i++] = 1;
    msg->options[i++] = DHCP_DISCOVER;

    /* Parameter request list */
    msg->options[i++] = DHCP_OPT_PARAM_REQ;
    msg->options[i++] = 4;
    msg->options[i++] = DHCP_OPT_SUBNET;
    msg->options[i++] = DHCP_OPT_ROUTER;
    msg->options[i++] = DHCP_OPT_DNS;
    msg->options[i++] = DHCP_OPT_LEASE_TIME;

    /* Hostname */
    msg->options[i++] = DHCP_OPT_HOSTNAME;
    msg->options[i++] = 4;
    msg->options[i++] = 'z';
    msg->options[i++] = 'e';
    msg->options[i++] = 'o';
    msg->options[i++] = 's';

    msg->options[i++] = DHCP_OPT_END;

    return (int)sizeof(struct dhcp_msg);
}

static int dhcp_build_request(uint8_t *buf, struct ipv4_addr offered_ip,
                               struct ipv4_addr server_ip) {
    struct dhcp_msg *msg = (struct dhcp_msg *)buf;
    mem_zero(msg, sizeof(*msg));

    msg->op    = 1;
    msg->htype = 1;
    msg->hlen  = 6;
    msg->xid   = htonl(g_xid);
    msg->flags = htons(0x8000);
    msg->magic = htonl(DHCP_MAGIC);

    mem_copy(msg->chaddr, g_net.mac.b, 6);

    int i = 0;
    /* Message type = REQUEST */
    msg->options[i++] = DHCP_OPT_MSG_TYPE;
    msg->options[i++] = 1;
    msg->options[i++] = DHCP_REQUEST;

    /* Requested IP */
    msg->options[i++] = DHCP_OPT_REQUESTED_IP;
    msg->options[i++] = 4;
    msg->options[i++] = offered_ip.b[0];
    msg->options[i++] = offered_ip.b[1];
    msg->options[i++] = offered_ip.b[2];
    msg->options[i++] = offered_ip.b[3];

    /* Server identifier */
    msg->options[i++] = DHCP_OPT_SERVER_ID;
    msg->options[i++] = 4;
    msg->options[i++] = server_ip.b[0];
    msg->options[i++] = server_ip.b[1];
    msg->options[i++] = server_ip.b[2];
    msg->options[i++] = server_ip.b[3];

    /* Hostname */
    msg->options[i++] = DHCP_OPT_HOSTNAME;
    msg->options[i++] = 4;
    msg->options[i++] = 'z';
    msg->options[i++] = 'e';
    msg->options[i++] = 'o';
    msg->options[i++] = 's';

    msg->options[i++] = DHCP_OPT_END;

    return (int)sizeof(struct dhcp_msg);
}

/* ── UDP callback ── */

static void dhcp_recv(struct ipv4_addr src, uint16_t src_port,
                      const void *data, uint16_t len)
{
    (void)src;
    (void)src_port;

    if (len < 236) return;  /* Too short for DHCP */

    const struct dhcp_msg *msg = (const struct dhcp_msg *)data;

    /* Check magic cookie */
    if (ntohl(msg->magic) != DHCP_MAGIC) return;

    /* Check transaction ID */
    if (ntohl(msg->xid) != g_xid) return;

    /* Check it's a reply */
    if (msg->op != 2) return;

    int type = dhcp_msg_type(msg);

    if (type == DHCP_OFFER && !dhcp_got_offer) {
        mem_copy(&dhcp_reply, msg, sizeof(dhcp_reply));
        dhcp_got_offer = 1;
    }
    else if (type == DHCP_ACK && !dhcp_got_ack) {
        mem_copy(&dhcp_reply, msg, sizeof(dhcp_reply));
        dhcp_got_ack = 1;
    }
}

/* ── Send DHCP via raw UDP broadcast ── */

/*
 * DHCP is special: we send from 0.0.0.0:68 to 255.255.255.255:67.
 * The normal ip_send() requires a source IP and ARP, neither of which
 * we have yet. So we build the full frame manually.
 */
static void dhcp_send_broadcast(const void *dhcp_data, int dhcp_len)
{
    uint8_t frame[NET_BUF_SIZE];

    /* Ethernet header — broadcast */
    struct eth_hdr *eth = (struct eth_hdr *)frame;
    for (int i = 0; i < 6; i++) eth->dst.b[i] = 0xFF;
    mem_copy(&eth->src, &g_net.mac, 6);
    eth->ethertype = htons(ETH_TYPE_IP);

    /* IPv4 header */
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(frame + sizeof(struct eth_hdr));
    mem_zero(ip, sizeof(*ip));
    ip->ver_ihl    = 0x45;
    ip->ttl        = 64;
    ip->protocol   = IP_PROTO_UDP;
    ip->total_len  = htons((uint16_t)(sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr) + dhcp_len));
    /* src = 0.0.0.0, dst = 255.255.255.255 */
    mem_zero(&ip->src, 4);
    for (int i = 0; i < 4; i++) ip->dst.b[i] = 255;
    ip->checksum = net_checksum(ip, sizeof(struct ipv4_hdr));

    /* UDP header */
    struct udp_hdr *udp = (struct udp_hdr *)((uint8_t *)ip + sizeof(struct ipv4_hdr));
    udp->src_port = htons(DHCP_CLIENT_PORT);
    udp->dst_port = htons(DHCP_SERVER_PORT);
    udp->length   = htons((uint16_t)(sizeof(struct udp_hdr) + dhcp_len));
    udp->checksum = 0;  /* Optional for IPv4 UDP */

    /* DHCP payload */
    mem_copy((uint8_t *)udp + sizeof(struct udp_hdr), dhcp_data, dhcp_len);

    /* Total frame length */
    int total = (int)sizeof(struct eth_hdr) + (int)sizeof(struct ipv4_hdr) +
                (int)sizeof(struct udp_hdr) + dhcp_len;

    net_drv_send(frame, (uint16_t)total);
}

/* ── Parse offer/ack options into lease ── */

static void dhcp_parse_lease(const struct dhcp_msg *msg, struct dhcp_lease *lease)
{
    mem_zero(lease, sizeof(*lease));

    /* yiaddr = our assigned IP */
    lease->ip = u32_to_ip(ntohl(msg->yiaddr));

    /* Parse options for subnet, router, DNS, lease time, server ID */
    int len;
    const uint8_t *val;

    val = dhcp_find_option(msg->options, 312, DHCP_OPT_SUBNET, &len);
    if (val && len >= 4) { mem_copy(&lease->netmask, val, 4); }

    val = dhcp_find_option(msg->options, 312, DHCP_OPT_ROUTER, &len);
    if (val && len >= 4) { mem_copy(&lease->gateway, val, 4); }

    val = dhcp_find_option(msg->options, 312, DHCP_OPT_DNS, &len);
    if (val && len >= 4) { mem_copy(&lease->dns, val, 4); }

    val = dhcp_find_option(msg->options, 312, DHCP_OPT_SERVER_ID, &len);
    if (val && len >= 4) { mem_copy(&lease->server, val, 4); }

    val = dhcp_find_option(msg->options, 312, DHCP_OPT_LEASE_TIME, &len);
    if (val && len >= 4) {
        lease->lease_time = ((uint32_t)val[0] << 24) | ((uint32_t)val[1] << 16) |
                            ((uint32_t)val[2] << 8) | val[3];
    }

    lease->valid = 1;
}

/* ── Public API ── */

int dhcp_discover(void)
{
    g_xid = dhcp_xid();
    dhcp_got_offer = 0;
    dhcp_got_ack = 0;
    mem_zero(&g_lease, sizeof(g_lease));

    /* Bind UDP port 68 for DHCP replies */
    udp_bind(DHCP_CLIENT_PORT, dhcp_recv);

    kputs("DHCP: discovering...\n");

    /* ── Phase 1: DISCOVER → OFFER ── */

    uint8_t msg_buf[sizeof(struct dhcp_msg)];
    int attempts = 3;
    int timeout_ms = 1000;

    for (int try = 0; try < attempts && !dhcp_got_offer; try++) {
        int len = dhcp_build_discover(msg_buf);
        dhcp_send_broadcast(msg_buf, len);
        net_poll_wait((uint32_t)timeout_ms);
        timeout_ms *= 2;  /* Exponential backoff */
    }

    if (!dhcp_got_offer) {
        kputs("DHCP: no offer received, using fallback\n");
        return -1;
    }

    /* Parse the offer */
    struct dhcp_lease offer;
    dhcp_parse_lease(&dhcp_reply, &offer);

    kputs("DHCP: offered ");
    kput_dec(offer.ip.b[0]); kputs(".");
    kput_dec(offer.ip.b[1]); kputs(".");
    kput_dec(offer.ip.b[2]); kputs(".");
    kput_dec(offer.ip.b[3]); kputs("\n");

    /* ── Phase 2: REQUEST → ACK ── */

    timeout_ms = 1000;

    for (int try = 0; try < attempts && !dhcp_got_ack; try++) {
        int len = dhcp_build_request(msg_buf, offer.ip, offer.server);
        dhcp_send_broadcast(msg_buf, len);
        net_poll_wait((uint32_t)timeout_ms);
        timeout_ms *= 2;
    }

    if (!dhcp_got_ack) {
        kputs("DHCP: no ack received, using fallback\n");
        return -1;
    }

    /* Parse the ACK — this is our final lease */
    dhcp_parse_lease(&dhcp_reply, &g_lease);

    /* ── Apply to global config ── */
    g_net.ip      = g_lease.ip;
    g_net.netmask = g_lease.netmask;
    g_net.gateway = g_lease.gateway;
    g_net.dns     = g_lease.dns;

    kputs("DHCP: bound ");
    kput_dec(g_net.ip.b[0]); kputs(".");
    kput_dec(g_net.ip.b[1]); kputs(".");
    kput_dec(g_net.ip.b[2]); kputs(".");
    kput_dec(g_net.ip.b[3]);
    kputs(" gw ");
    kput_dec(g_net.gateway.b[0]); kputs(".");
    kput_dec(g_net.gateway.b[1]); kputs(".");
    kput_dec(g_net.gateway.b[2]); kputs(".");
    kput_dec(g_net.gateway.b[3]);
    kputs(" dns ");
    kput_dec(g_net.dns.b[0]); kputs(".");
    kput_dec(g_net.dns.b[1]); kputs(".");
    kput_dec(g_net.dns.b[2]); kputs(".");
    kput_dec(g_net.dns.b[3]);
    kputs(" lease ");
    kput_dec((int)g_lease.lease_time);
    kputs("s\n");

    return 0;
}

const struct dhcp_lease *dhcp_get_lease(void)
{
    return &g_lease;
}

/* ── Async DHCP: non-blocking start + a state machine serviced once per
 * scheduler tick. This replaces the blocking dhcp_discover() busy-loop on the
 * boot path so network I/O is pumped BY the scheduler (chain-resolution) rather
 * than blocking before it exists. ── */
enum { DHCP_ST_IDLE = 0, DHCP_ST_DISCOVER, DHCP_ST_REQUEST, DHCP_ST_BOUND, DHCP_ST_FAILED };
static int               g_dhcp_st = DHCP_ST_IDLE;
static uint64_t          g_dhcp_deadline;
static int               g_dhcp_tries;
static struct dhcp_lease g_dhcp_offer;

static uint64_t dhcp_deadline_in(uint32_t secs)
{
    extern uint64_t timer_read_tsc(void); extern uint64_t timer_tsc_freq(void);
    uint64_t f = timer_tsc_freq(); if (!f) f = 2000000000ULL;
    return timer_read_tsc() + (uint64_t)secs * f;
}

static void dhcp_apply_lease(void)
{
    g_net.ip      = g_lease.ip;
    g_net.netmask = g_lease.netmask;
    g_net.gateway = g_lease.gateway;
    g_net.dns     = g_lease.dns;
    kputs("DHCP: bound ");
    kput_dec(g_net.ip.b[0]); kputs("."); kput_dec(g_net.ip.b[1]); kputs(".");
    kput_dec(g_net.ip.b[2]); kputs("."); kput_dec(g_net.ip.b[3]);
    kputs(" gw ");
    kput_dec(g_net.gateway.b[0]); kputs("."); kput_dec(g_net.gateway.b[1]); kputs(".");
    kput_dec(g_net.gateway.b[2]); kputs("."); kput_dec(g_net.gateway.b[3]);
    kputs(" dns ");
    kput_dec(g_net.dns.b[0]); kputs("."); kput_dec(g_net.dns.b[1]); kputs(".");
    kput_dec(g_net.dns.b[2]); kputs("."); kput_dec(g_net.dns.b[3]);
    kputs("\n");
}

/* Kick off DHCP without blocking: bind the receiver, send DISCOVER, return.
 * dhcp_service() (called each scheduler tick) drives it to completion. */
void dhcp_start(void)
{
    g_xid = dhcp_xid();
    dhcp_got_offer = 0;
    dhcp_got_ack   = 0;
    mem_zero(&g_lease, sizeof(g_lease));
    udp_bind(DHCP_CLIENT_PORT, dhcp_recv);

    uint8_t buf[sizeof(struct dhcp_msg)];
    int len = dhcp_build_discover(buf);
    dhcp_send_broadcast(buf, len);
    kputs("DHCP: discovering (async)...\n");
    g_dhcp_st       = DHCP_ST_DISCOVER;
    g_dhcp_tries    = 1;
    g_dhcp_deadline = dhcp_deadline_in(2);
}

/* Advance the DHCP state machine. Call after net_poll() has fed dhcp_recv. */
void dhcp_service(void)
{
    if (g_dhcp_st != DHCP_ST_DISCOVER && g_dhcp_st != DHCP_ST_REQUEST)
        return;

    extern uint64_t timer_read_tsc(void);
    uint64_t now = timer_read_tsc();
    uint8_t  buf[sizeof(struct dhcp_msg)];

    if (g_dhcp_st == DHCP_ST_DISCOVER) {
        if (dhcp_got_offer) {
            dhcp_parse_lease(&dhcp_reply, &g_dhcp_offer);
            int len = dhcp_build_request(buf, g_dhcp_offer.ip, g_dhcp_offer.server);
            dhcp_send_broadcast(buf, len);
            g_dhcp_st       = DHCP_ST_REQUEST;
            g_dhcp_deadline = dhcp_deadline_in(2);
        } else if (now >= g_dhcp_deadline) {
            if (g_dhcp_tries >= 5) {
                kputs("DHCP: no offer (async) -- no IP\n");
                g_net.up = 0; g_dhcp_st = DHCP_ST_FAILED; return;
            }
            int len = dhcp_build_discover(buf);
            dhcp_send_broadcast(buf, len);
            g_dhcp_tries++;
            g_dhcp_deadline = dhcp_deadline_in(2);
        }
    } else { /* DHCP_ST_REQUEST */
        if (dhcp_got_ack) {
            dhcp_parse_lease(&dhcp_reply, &g_lease);
            dhcp_apply_lease();
            g_dhcp_st = DHCP_ST_BOUND;
        } else if (now >= g_dhcp_deadline) {
            if (g_dhcp_tries >= 8) { g_dhcp_st = DHCP_ST_FAILED; return; }
            int len = dhcp_build_request(buf, g_dhcp_offer.ip, g_dhcp_offer.server);
            dhcp_send_broadcast(buf, len);
            g_dhcp_tries++;
            g_dhcp_deadline = dhcp_deadline_in(2);
        }
    }
}

int dhcp_is_bound(void) { return g_dhcp_st == DHCP_ST_BOUND; }
