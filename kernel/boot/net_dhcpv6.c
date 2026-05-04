/*
 * Zeos -- DHCPv6 Client
 *
 * Speaks RFC 8415 SOLICIT -> ADVERTISE -> REQUEST -> REPLY over UDP/IPv6
 * with the destination All-DHCP-Servers (ff02::1:2) at port 547.
 * Uses our link-local as source. Lease honors valid_lifetime; renewal
 * happens on the same shape as v4 (caller re-invokes when lifetime
 * expires).
 */

#include "net_dhcpv6.h"
#include "net_ipv6.h"
#include "net.h"
#include "kprint.h"

/* ── Module state ─────────────────────────────── */

static struct dhcpv6_lease s_lease;
static volatile int        s_got_advertise = 0;
static volatile int        s_got_reply     = 0;
static uint8_t             s_server_duid[128];
static int                 s_server_duid_len = 0;
static struct ipv6_addr    s_advertised_addr;
static uint32_t            s_advertised_t1, s_advertised_t2;
static uint32_t            s_advertised_pref, s_advertised_valid;
static uint8_t             s_xid[3];

/* Our DUID-LL (link-layer address based on MAC). */
static uint8_t  s_client_duid[14];   /* 2 + 2 + 2 + 6 */
static int      s_client_duid_len = 0;

static void mz(void *p, int n) { uint8_t *b=p; for (int i=0;i<n;i++) b[i]=0; }
static void mc(void *d, const void *s, int n){ uint8_t *dd=d; const uint8_t *ss=s; for(int i=0;i<n;i++) dd[i]=ss[i]; }
static int  meq(const void *a, const void *b, int n){
    const uint8_t *aa=a, *bb=b; for(int i=0;i<n;i++) if (aa[i]!=bb[i]) return 0; return 1;
}

static void build_client_duid(void)
{
    /* DUID-LL: type=3(2) | hw=1 (Ethernet)(2) | LL addr (6) */
    s_client_duid[0] = 0x00; s_client_duid[1] = 0x03;
    s_client_duid[2] = 0x00; s_client_duid[3] = 0x01;
    mc(s_client_duid + 4, g_net.mac.b, 6);
    s_client_duid_len = 10;
}

/* ── Option helpers ───────────────────────────── */

static int opt_put(uint8_t *buf, int pos, uint16_t code, const void *val, uint16_t len)
{
    buf[pos++] = (uint8_t)(code >> 8);
    buf[pos++] = (uint8_t)(code & 0xFF);
    buf[pos++] = (uint8_t)(len >> 8);
    buf[pos++] = (uint8_t)(len & 0xFF);
    if (val && len) mc(buf + pos, val, len);
    return pos + len;
}

static const uint8_t *opt_find(const uint8_t *opts, int total, uint16_t code, int *out_len)
{
    int i = 0;
    while (i + 4 <= total) {
        uint16_t c = ((uint16_t)opts[i] << 8) | opts[i+1];
        uint16_t l = ((uint16_t)opts[i+2] << 8) | opts[i+3];
        if (i + 4 + l > total) return 0;
        if (c == code) { *out_len = l; return opts + i + 4; }
        i += 4 + l;
    }
    return 0;
}

/* ── UDP/IPv6 send ────────────────────────────── */

/* We don't have a full UDP6 send routine elsewhere; build it inline.
 * UDP checksum over IPv6 pseudo-header is mandatory. */
extern uint16_t ipv6_pseudo_checksum_external(const struct ipv6_addr *src,
                                              const struct ipv6_addr *dst,
                                              uint8_t next_header,
                                              const void *data, uint32_t len);

static uint16_t udp6_pseudo_checksum(const struct ipv6_addr *src,
                                     const struct ipv6_addr *dst,
                                     const void *udp_pkt, uint32_t udp_len)
{
    /* Re-compute here to avoid touching net_ipv6.c API surface. */
    uint32_t sum = 0;
    const uint16_t *p;
    p = (const uint16_t *)src->bytes;
    for (int i = 0; i < 8; i++) sum += p[i];
    p = (const uint16_t *)dst->bytes;
    for (int i = 0; i < 8; i++) sum += p[i];
    uint32_t lh = htonl(udp_len);
    sum += (lh & 0xFFFF);
    sum += (lh >> 16);
    sum += htons((uint16_t)17);
    p = (const uint16_t *)udp_pkt;
    uint32_t rem = udp_len;
    while (rem > 1) { sum += *p++; rem -= 2; }
    if (rem == 1) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    uint16_t r = (uint16_t)~sum;
    return r ? r : 0xFFFF;
}

static int udp6_send_dhcp(struct ipv6_addr dst, const void *payload, uint16_t plen)
{
    uint8_t pkt[1280];
    if ((unsigned)plen + 8 > sizeof(pkt)) return -1;
    /* UDP header */
    pkt[0] = (DHCPV6_CLIENT_PORT >> 8) & 0xFF;
    pkt[1] = DHCPV6_CLIENT_PORT & 0xFF;
    pkt[2] = (DHCPV6_SERVER_PORT >> 8) & 0xFF;
    pkt[3] = DHCPV6_SERVER_PORT & 0xFF;
    uint16_t ulen = (uint16_t)(8 + plen);
    pkt[4] = (uint8_t)(ulen >> 8);
    pkt[5] = (uint8_t)(ulen & 0xFF);
    pkt[6] = 0; pkt[7] = 0;
    mc(pkt + 8, payload, plen);

    struct ipv6_addr src = ipv6_pick_source(dst);
    uint16_t ck = udp6_pseudo_checksum(&src, &dst, pkt, ulen);
    pkt[6] = (uint8_t)(ck & 0xFF);
    pkt[7] = (uint8_t)(ck >> 8);

    return ipv6_send(dst, IPV6_NH_UDP, pkt, ulen);
}

/* ── UDP6 RX hook (called from net_ipv6.c) ────── */

void udp6_process(const struct ipv6_addr *src, const struct ipv6_addr *dst,
                  const void *udp_pkt, uint16_t udp_len);

void udp6_process(const struct ipv6_addr *src, const struct ipv6_addr *dst,
                  const void *udp_pkt, uint16_t udp_len)
{
    (void)dst;
    if (udp_len < 8) return;
    const uint8_t *u = (const uint8_t *)udp_pkt;
    uint16_t src_port = ((uint16_t)u[0] << 8) | u[1];
    uint16_t dst_port = ((uint16_t)u[2] << 8) | u[3];

    if (dst_port != DHCPV6_CLIENT_PORT) return;
    (void)src_port;
    (void)src;

    const uint8_t *body = u + 8;
    int blen = udp_len - 8;
    if (blen < 4) return;
    uint8_t msg_type = body[0];
    /* Match XID */
    if (body[1] != s_xid[0] || body[2] != s_xid[1] || body[3] != s_xid[2]) return;

    const uint8_t *opts = body + 4;
    int opts_len = blen - 4;

    if (msg_type == DHCP6_ADVERTISE && !s_got_advertise) {
        int sl = 0;
        const uint8_t *sid = opt_find(opts, opts_len, DHCP6_OPT_SERVERID, &sl);
        if (!sid || sl > (int)sizeof(s_server_duid)) return;
        mc(s_server_duid, sid, sl);
        s_server_duid_len = sl;

        int ial = 0;
        const uint8_t *ia = opt_find(opts, opts_len, DHCP6_OPT_IA_NA, &ial);
        if (!ia || ial < 12) return;
        s_advertised_t1 = ((uint32_t)ia[4] << 24) | ((uint32_t)ia[5] << 16) |
                          ((uint32_t)ia[6] << 8)  | ia[7];
        s_advertised_t2 = ((uint32_t)ia[8] << 24) | ((uint32_t)ia[9] << 16) |
                          ((uint32_t)ia[10] << 8) | ia[11];
        const uint8_t *iaopts = ia + 12;
        int iaolen = ial - 12;
        int al = 0;
        const uint8_t *aopt = opt_find(iaopts, iaolen, DHCP6_OPT_IAADDR, &al);
        if (!aopt || al < 24) return;
        mc(s_advertised_addr.bytes, aopt, 16);
        s_advertised_pref  = ((uint32_t)aopt[16] << 24) | ((uint32_t)aopt[17] << 16) |
                             ((uint32_t)aopt[18] << 8)  | aopt[19];
        s_advertised_valid = ((uint32_t)aopt[20] << 24) | ((uint32_t)aopt[21] << 16) |
                             ((uint32_t)aopt[22] << 8)  | aopt[23];
        s_got_advertise = 1;
    }
    else if (msg_type == DHCP6_REPLY && !s_got_reply) {
        int sl = 0;
        const uint8_t *sid = opt_find(opts, opts_len, DHCP6_OPT_SERVERID, &sl);
        if (!sid || sl != s_server_duid_len ||
            !meq(sid, s_server_duid, sl)) return;
        int ial = 0;
        const uint8_t *ia = opt_find(opts, opts_len, DHCP6_OPT_IA_NA, &ial);
        if (!ia || ial < 12) return;
        const uint8_t *iaopts = ia + 12;
        int iaolen = ial - 12;
        int al = 0;
        const uint8_t *aopt = opt_find(iaopts, iaolen, DHCP6_OPT_IAADDR, &al);
        if (!aopt || al < 24) return;
        mc(s_lease.addr.bytes, aopt, 16);
        s_lease.preferred_lifetime = ((uint32_t)aopt[16] << 24) |
                                     ((uint32_t)aopt[17] << 16) |
                                     ((uint32_t)aopt[18] << 8)  | aopt[19];
        s_lease.valid_lifetime     = ((uint32_t)aopt[20] << 24) |
                                     ((uint32_t)aopt[21] << 16) |
                                     ((uint32_t)aopt[22] << 8)  | aopt[23];
        s_lease.valid = 1;
        s_got_reply = 1;
    }
}

/* ── Build messages ──────────────────────────── */

static int build_solicit(uint8_t *buf)
{
    buf[0] = DHCP6_SOLICIT;
    buf[1] = s_xid[0]; buf[2] = s_xid[1]; buf[3] = s_xid[2];
    int p = 4;
    /* Elapsed time */
    uint8_t et[2] = {0, 0};
    p = opt_put(buf, p, DHCP6_OPT_ELAPSED, et, 2);
    /* Client ID */
    p = opt_put(buf, p, DHCP6_OPT_CLIENTID, s_client_duid, s_client_duid_len);
    /* IA_NA: 4 IAID + 4 T1 + 4 T2 */
    uint8_t ia[12]; mz(ia, 12);
    ia[0] = 'Z'; ia[1] = 'E'; ia[2] = 'O'; ia[3] = 'S';
    p = opt_put(buf, p, DHCP6_OPT_IA_NA, ia, 12);
    /* ORO: ask for DNS */
    uint8_t oro[2] = {0, DHCP6_OPT_DNS};
    p = opt_put(buf, p, DHCP6_OPT_ORO, oro, 2);
    return p;
}

static int build_request(uint8_t *buf)
{
    buf[0] = DHCP6_REQUEST;
    buf[1] = s_xid[0]; buf[2] = s_xid[1]; buf[3] = s_xid[2];
    int p = 4;
    uint8_t et[2] = {0, 0};
    p = opt_put(buf, p, DHCP6_OPT_ELAPSED, et, 2);
    p = opt_put(buf, p, DHCP6_OPT_CLIENTID, s_client_duid, s_client_duid_len);
    p = opt_put(buf, p, DHCP6_OPT_SERVERID, s_server_duid, s_server_duid_len);
    /* IA_NA echoing the offered address */
    uint8_t ia[12 + 4 + 24];
    mz(ia, sizeof(ia));
    ia[0]='Z'; ia[1]='E'; ia[2]='O'; ia[3]='S';
    /* T1, T2 */
    ia[4] = (uint8_t)(s_advertised_t1 >> 24);
    ia[5] = (uint8_t)(s_advertised_t1 >> 16);
    ia[6] = (uint8_t)(s_advertised_t1 >> 8);
    ia[7] = (uint8_t)(s_advertised_t1);
    ia[8] = (uint8_t)(s_advertised_t2 >> 24);
    ia[9] = (uint8_t)(s_advertised_t2 >> 16);
    ia[10] = (uint8_t)(s_advertised_t2 >> 8);
    ia[11] = (uint8_t)(s_advertised_t2);
    /* Sub-option IAADDR */
    ia[12] = 0; ia[13] = DHCP6_OPT_IAADDR;
    ia[14] = 0; ia[15] = 24;
    mc(ia + 16, s_advertised_addr.bytes, 16);
    ia[32] = (uint8_t)(s_advertised_pref >> 24);
    ia[33] = (uint8_t)(s_advertised_pref >> 16);
    ia[34] = (uint8_t)(s_advertised_pref >> 8);
    ia[35] = (uint8_t)(s_advertised_pref);
    ia[36] = (uint8_t)(s_advertised_valid >> 24);
    ia[37] = (uint8_t)(s_advertised_valid >> 16);
    ia[38] = (uint8_t)(s_advertised_valid >> 8);
    ia[39] = (uint8_t)(s_advertised_valid);
    p = opt_put(buf, p, DHCP6_OPT_IA_NA, ia, sizeof(ia));
    return p;
}

/* ── Public ──────────────────────────────────── */

int dhcpv6_run(void)
{
    s_lease.valid = 0;
    s_got_advertise = 0;
    s_got_reply = 0;
    s_server_duid_len = 0;
    if (!s_client_duid_len) build_client_duid();

    /* Generate XID from TSC */
    uint64_t t;
    { uint32_t lo, hi; __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)); t = ((uint64_t)hi<<32)|lo; }
    s_xid[0] = (uint8_t)(t >> 16);
    s_xid[1] = (uint8_t)(t >> 8);
    s_xid[2] = (uint8_t)(t);

    struct ipv6_addr all_servers = {{0xFF,0x02,0,0,0,0,0,0,0,0,0,0,0x00,0x01,0x00,0x02}};

    kputs("DHCPv6: SOLICIT...\n");

    uint8_t pkt[512];
    int attempts = 3;
    int delay_ms = 1000;
    for (int i = 0; i < attempts && !s_got_advertise; i++) {
        int len = build_solicit(pkt);
        udp6_send_dhcp(all_servers, pkt, (uint16_t)len);
        net_poll_wait((uint32_t)delay_ms);
        delay_ms *= 2;
    }
    if (!s_got_advertise) { kputs("DHCPv6: no advertise\n"); return -1; }

    kputs("DHCPv6: REQUEST...\n");
    delay_ms = 1000;
    for (int i = 0; i < attempts && !s_got_reply; i++) {
        int len = build_request(pkt);
        udp6_send_dhcp(all_servers, pkt, (uint16_t)len);
        net_poll_wait((uint32_t)delay_ms);
        delay_ms *= 2;
    }
    if (!s_got_reply) { kputs("DHCPv6: no reply\n"); return -1; }

    /* Install address */
    for (int i = 0; i < IPV6_ADDR_MAX; i++) {
        if (!g_net6.addrs[i].valid) {
            g_net6.addrs[i].addr = s_lease.addr;
            g_net6.addrs[i].prefix_len = 128;
            g_net6.addrs[i].is_dhcp = 1;
            g_net6.addrs[i].valid = 1;
            break;
        }
    }

    char buf[48];
    ipv6_format(s_lease.addr, buf);
    kputs("DHCPv6: bound ");
    kputs(buf);
    kputs("\n");
    return 0;
}

const struct dhcpv6_lease *dhcpv6_get_lease(void) { return &s_lease; }
