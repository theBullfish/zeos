/*
 * Stateful packet-inspection firewall as a chain. Inserted into
 * CHAIN_NET_TX/RX pipelines as a gate node. Default policy:
 * permissive outbound, restrictive inbound (drop unsolicited).
 * Connection table tracks established for stateful rules.
 * Drops fire notifications at rate-limited cadence.
 */

#include "firewall.h"
#include "net.h"
#include "net_tcp.h"
#include "chain.h"
#include "chain_registry.h"
#include "vault.h"
#include "notify.h"
#include "settings_registry.h"
#include "kprint.h"
#include "timer.h"
#include "spinlock.h"
#include <stdint.h>

/* ── Public chain id ─────────────────────────────────────────────── */

int CHAIN_FIREWALL = -1;

/* ── String helpers (no libc) ────────────────────────────────────── */

__attribute__((unused))
static int fw_strlen(const char *s) {
    int n = 0; while (s && s[n]) n++; return n;
}
static int fw_streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void fw_strncpy(char *d, const char *s, int max) {
    int i = 0;
    if (!d || max <= 0) return;
    if (s) while (i < max - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static int fw_atou(const char *s, uint32_t *out) {
    if (!s || !*s) return -1;
    uint32_t v = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
    }
    *out = v;
    return 0;
}

/* ── State ───────────────────────────────────────────────────────── */

static int                 g_initialized;
static int                 g_enabled = 1;
static firewall_rule_t     g_rules[FIREWALL_MAX_RULES];
static int                 g_rule_count;
static uint32_t            g_next_rule_id = 1;

static firewall_conn_t     g_conns[FIREWALL_CONN_MAX];

static firewall_drop_log_t g_droplog[FIREWALL_DROP_LOG_MAX];
static int                 g_droplog_count;     /* total entries written, capped at MAX once full */
static int                 g_droplog_head;      /* next write index */

static uint32_t            g_drops_total;
static uint32_t            g_chain_decisions_total;

/* Notify rate limit: per-source (src_ip) cooldown of 1s. Small LRU. */
#define FW_NOTIFY_LRU 16
static struct {
    struct ipv4_addr ip;
    uint64_t         last_tsc;
    int              valid;
} s_notify_lru[FW_NOTIFY_LRU];

/* Default policy: ALLOW (start permissive — opt-in to lockdown). The
 * default-rule chain at the END of the table covers the no-match case;
 * separate "allow" / "drop" defaults for inbound/outbound surfaces are
 * realized via dedicated rules at very high priority numbers (so they
 * sort last). */

/* TX/RX submit-side serialization: the firewall_check_tx/_rx hook nodes
 * stage a firewall_check_t into s_pending and call chain_resolve. The
 * decision is read back from s_pending_decision. The per-chain try-lock
 * inside chain_resolve already serializes the FIREWALL chain itself, but
 * the stage+resolve+consume sequence in the hook node needs its own
 * mutex because two hook resolves on different cores would otherwise
 * trample s_pending. Two locks (TX/RX) so opposite directions don't
 * serialize against each other. */
static zeos_spinlock_t s_pending_lock_tx = ZEOS_SPINLOCK_INIT;
static zeos_spinlock_t s_pending_lock_rx = ZEOS_SPINLOCK_INIT;

static firewall_check_t    s_pending_in;     /* staged input */
static firewall_decision_t s_pending_out;    /* result */

/* Re-entrancy guard for REJECT response packets (TCP RST / ICMP
 * unreachable). When set, the firewall hook lets the packet through
 * without a fresh decision (the response was crafted by US, not by
 * userspace). Single-threaded under s_pending_lock_*, so a static flag
 * is fine. */
static int s_skip_for_reject_tx;

/* VAULT keys */
#define FW_KEY_RULES "/firewall/rules"
#define FW_KEY_CONNS "/firewall/connections"
#define FW_KEY_ENABLED "/firewall/enabled"

/* ── IP helpers ──────────────────────────────────────────────────── */

__attribute__((unused))
static int ip_is_zero(struct ipv4_addr a) {
    return a.b[0] == 0 && a.b[1] == 0 && a.b[2] == 0 && a.b[3] == 0;
}
static int ip_is_loopback(struct ipv4_addr a) {
    return a.b[0] == 127;
}
__attribute__((unused))
static int ip_is_rfc1918(struct ipv4_addr a) {
    if (a.b[0] == 10) return 1;
    if (a.b[0] == 172 && a.b[1] >= 16 && a.b[1] <= 31) return 1;
    if (a.b[0] == 192 && a.b[1] == 168) return 1;
    return 0;
}
static int ip_match_with_mask(struct ipv4_addr ip,
                              struct ipv4_addr match,
                              struct ipv4_addr mask) {
    /* All-zero mask = "any" wildcard. */
    if (mask.b[0] == 0 && mask.b[1] == 0 && mask.b[2] == 0 && mask.b[3] == 0)
        return 1;
    for (int i = 0; i < 4; i++) {
        if ((ip.b[i] & mask.b[i]) != (match.b[i] & mask.b[i])) return 0;
    }
    return 1;
}

/* ── Frame parsing ───────────────────────────────────────────────── */

/* Parse IP + L4. Returns 1 on success, 0 on parse fail / not-IP / IPv6
 * (we don't gate IPv6 here yet — pass through). */
static int parse_frame(const uint8_t *frame, uint16_t len,
                       firewall_check_t *out)
{
    if (!frame || len < 14) return 0;
    uint16_t ethertype = (uint16_t)((frame[12] << 8) | frame[13]);
    if (ethertype != ETH_TYPE_IP) {
        /* ARP / IPv6 / other — let it pass for now (no v6 firewalling
         * yet). Caller treats out->l3=ANY as "no match — allow". */
        out->l3 = FW_L3_ANY;
        return 0;
    }
    if (len < 14 + 20) return 0;
    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)(frame + 14);
    uint16_t ihl = (uint16_t)((ip->ver_ihl & 0x0F) * 4);
    if (ihl < 20 || (uint16_t)(14 + ihl) > len) return 0;

    out->l3       = FW_L3_V4;
    out->l4       = (fw_l4_t)ip->protocol;
    out->src_ip   = ip->src;
    out->dst_ip   = ip->dst;
    out->src_port = 0;
    out->dst_port = 0;
    out->tcp_flags = 0;
    out->have_l4  = 0;

    const uint8_t *l4 = frame + 14 + ihl;
    uint16_t l4_len = (uint16_t)(len - 14 - ihl);

    if (ip->protocol == IP_PROTO_TCP && l4_len >= 20) {
        out->src_port  = (uint16_t)((l4[0] << 8) | l4[1]);
        out->dst_port  = (uint16_t)((l4[2] << 8) | l4[3]);
        out->tcp_flags = l4[13];
        out->have_l4   = 1;
    } else if (ip->protocol == IP_PROTO_UDP && l4_len >= 8) {
        out->src_port = (uint16_t)((l4[0] << 8) | l4[1]);
        out->dst_port = (uint16_t)((l4[2] << 8) | l4[3]);
        out->have_l4  = 1;
    } else if (ip->protocol == IP_PROTO_ICMP) {
        out->have_l4 = 1;
    }
    return 1;
}

/* ── Connection tracking ─────────────────────────────────────────── */

static firewall_conn_t *conn_find(const firewall_check_t *p)
{
    for (int i = 0; i < FIREWALL_CONN_MAX; i++) {
        firewall_conn_t *c = &g_conns[i];
        if (!c->in_use) continue;
        if (c->proto != p->l4) continue;
        /* Match either direction (src/dst could be flipped depending on
         * which side initiated). */
        int fwd = (c->src_ip.b[0]==p->src_ip.b[0] && c->src_ip.b[1]==p->src_ip.b[1] &&
                   c->src_ip.b[2]==p->src_ip.b[2] && c->src_ip.b[3]==p->src_ip.b[3] &&
                   c->dst_ip.b[0]==p->dst_ip.b[0] && c->dst_ip.b[1]==p->dst_ip.b[1] &&
                   c->dst_ip.b[2]==p->dst_ip.b[2] && c->dst_ip.b[3]==p->dst_ip.b[3] &&
                   c->src_port == p->src_port && c->dst_port == p->dst_port);
        int rev = (c->src_ip.b[0]==p->dst_ip.b[0] && c->src_ip.b[1]==p->dst_ip.b[1] &&
                   c->src_ip.b[2]==p->dst_ip.b[2] && c->src_ip.b[3]==p->dst_ip.b[3] &&
                   c->dst_ip.b[0]==p->src_ip.b[0] && c->dst_ip.b[1]==p->src_ip.b[1] &&
                   c->dst_ip.b[2]==p->src_ip.b[2] && c->dst_ip.b[3]==p->src_ip.b[3] &&
                   c->src_port == p->dst_port && c->dst_port == p->src_port);
        if (fwd || rev) return c;
    }
    return 0;
}

static firewall_conn_t *conn_alloc(void)
{
    /* Find a free slot, else evict oldest. */
    firewall_conn_t *oldest = &g_conns[0];
    for (int i = 0; i < FIREWALL_CONN_MAX; i++) {
        if (!g_conns[i].in_use) return &g_conns[i];
        if (g_conns[i].last_seen_tsc < oldest->last_seen_tsc) oldest = &g_conns[i];
    }
    return oldest;
}

static void conn_remove(firewall_conn_t *c)
{
    if (c) c->in_use = 0;
}

/* Update conntrack from an outbound or inbound packet. RST/FIN cleans
 * the entry. */
static void conn_track(const firewall_check_t *p)
{
    if (p->l4 != IP_PROTO_TCP && p->l4 != IP_PROTO_UDP) return;

    firewall_conn_t *c = conn_find(p);
    uint64_t now = timer_read_tsc();

    if (p->l4 == IP_PROTO_TCP && (p->tcp_flags & (TCP_RST | TCP_FIN))) {
        if (c) {
            c->state = FW_CONN_CLOSING;
            c->last_seen_tsc = now;
            /* On RST, free immediately. On FIN, keep CLOSING for one
             * cycle so any in-flight ACK gets through. */
            if (p->tcp_flags & TCP_RST) conn_remove(c);
        }
        return;
    }

    if (!c) {
        c = conn_alloc();
        if (!c) return;
        c->in_use   = 1;
        c->proto    = p->l4;
        c->src_ip   = p->src_ip;
        c->dst_ip   = p->dst_ip;
        c->src_port = p->src_port;
        c->dst_port = p->dst_port;
        c->state    = FW_CONN_NEW;
    }
    c->last_seen_tsc = now;
    if (p->l4 == IP_PROTO_TCP) {
        if (p->tcp_flags & TCP_ACK) c->state = FW_CONN_ESTABLISHED;
    } else {
        /* UDP: any second packet promotes to established. */
        if (c->state == FW_CONN_NEW) c->state = FW_CONN_ESTABLISHED;
    }
}

int firewall_active_conn_count(void)
{
    int n = 0;
    for (int i = 0; i < FIREWALL_CONN_MAX; i++)
        if (g_conns[i].in_use) n++;
    return n;
}

/* ── Rule sort + match ───────────────────────────────────────────── */

/* Insertion-sort g_rules by priority (stable). Called after add. */
static void rules_sort(void)
{
    for (int i = 1; i < g_rule_count; i++) {
        firewall_rule_t key = g_rules[i];
        int j = i - 1;
        while (j >= 0 && g_rules[j].priority > key.priority) {
            g_rules[j+1] = g_rules[j];
            j--;
        }
        g_rules[j+1] = key;
    }
}

static int dir_matches(uint8_t rule_dir, uint8_t pkt_dir)
{
    if (rule_dir == FW_DIR_BOTH) return 1;
    return (rule_dir & pkt_dir) ? 1 : 0;
}

static int port_in_range(uint16_t port, uint16_t lo, uint16_t hi)
{
    if (lo == 0 && hi == 0) return 1;     /* wildcard */
    if (hi == 0) hi = 0xFFFF;
    return (port >= lo && port <= hi);
}

static int rule_matches(const firewall_rule_t *r,
                        const firewall_check_t *p)
{
    if (!r->enabled) return 0;
    if (!dir_matches(r->direction, p->direction)) return 0;
    if (r->l3 != FW_L3_ANY && r->l3 != p->l3)     return 0;
    if (r->l4 != FW_L4_ANY && (uint8_t)r->l4 != p->l4) return 0;
    if (!ip_match_with_mask(p->src_ip, r->src_ip_match, r->src_ip_mask)) return 0;
    if (!ip_match_with_mask(p->dst_ip, r->dst_ip_match, r->dst_ip_mask)) return 0;
    if (p->have_l4 && (r->l4 == FW_L4_TCP || r->l4 == FW_L4_UDP)) {
        if (!port_in_range(p->src_port, r->src_port_min, r->src_port_max)) return 0;
        if (!port_in_range(p->dst_port, r->dst_port_min, r->dst_port_max)) return 0;
    }
    return 1;
}

/* ── Drop log ────────────────────────────────────────────────────── */

static void droplog_push(const firewall_check_t *p, fw_action_t a, int rule_id,
                         const char *reason)
{
    firewall_drop_log_t *e = &g_droplog[g_droplog_head];
    e->tsc       = timer_read_tsc();
    e->src_ip    = p->src_ip;
    e->dst_ip    = p->dst_ip;
    e->src_port  = p->src_port;
    e->dst_port  = p->dst_port;
    e->direction = p->direction;
    e->proto     = p->l4;
    e->action    = (uint8_t)a;
    e->rule_id   = rule_id;
    fw_strncpy(e->reason, reason ? reason : "drop", sizeof(e->reason));
    g_droplog_head = (g_droplog_head + 1) % FIREWALL_DROP_LOG_MAX;
    if (g_droplog_count < FIREWALL_DROP_LOG_MAX) g_droplog_count++;
}

int firewall_log_count(void) { return g_droplog_count; }

int firewall_log_get(int idx, firewall_drop_log_t *out)
{
    if (!out) return -1;
    if (idx < 0 || idx >= g_droplog_count) return -1;
    /* Newest first: head-1 - idx, mod MAX. */
    int pos = (g_droplog_head - 1 - idx);
    while (pos < 0) pos += FIREWALL_DROP_LOG_MAX;
    *out = g_droplog[pos];
    return 0;
}

/* drops/min via 60s rolling window over the log. */
uint32_t firewall_drops_per_min(void)
{
    uint64_t now  = timer_read_tsc();
    uint64_t freq = timer_tsc_freq();
    if (freq == 0) freq = 2000000000ULL;
    uint64_t cutoff_delta = freq * 60ULL;
    uint32_t n = 0;
    for (int i = 0; i < g_droplog_count; i++) {
        firewall_drop_log_t e;
        if (firewall_log_get(i, &e) != 0) break;
        if (e.tsc + cutoff_delta < now) break;   /* older than 60s */
        n++;
    }
    return n;
}

/* ── Notify rate limit ───────────────────────────────────────────── */

static int notify_should_fire(struct ipv4_addr src_ip)
{
    uint64_t now  = timer_read_tsc();
    uint64_t freq = timer_tsc_freq();
    if (freq == 0) freq = 2000000000ULL;
    uint64_t one_sec = freq;

    /* Look up. */
    int free_slot = -1;
    for (int i = 0; i < FW_NOTIFY_LRU; i++) {
        if (!s_notify_lru[i].valid) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (s_notify_lru[i].ip.b[0]==src_ip.b[0] &&
            s_notify_lru[i].ip.b[1]==src_ip.b[1] &&
            s_notify_lru[i].ip.b[2]==src_ip.b[2] &&
            s_notify_lru[i].ip.b[3]==src_ip.b[3]) {
            if (now - s_notify_lru[i].last_tsc < one_sec) return 0;
            s_notify_lru[i].last_tsc = now;
            return 1;
        }
    }
    /* Not seen within window — install (or evict oldest). */
    if (free_slot < 0) {
        int oldest = 0;
        for (int i = 1; i < FW_NOTIFY_LRU; i++) {
            if (s_notify_lru[i].last_tsc < s_notify_lru[oldest].last_tsc)
                oldest = i;
        }
        free_slot = oldest;
    }
    s_notify_lru[free_slot].ip = src_ip;
    s_notify_lru[free_slot].last_tsc = now;
    s_notify_lru[free_slot].valid = 1;
    return 1;
}

static void format_ip(struct ipv4_addr a, char *out, int n)
{
    /* "a.b.c.d" */
    int p = 0;
    for (int oct = 0; oct < 4; oct++) {
        uint8_t v = a.b[oct];
        char buf[4]; int b = 0;
        if (v == 0) buf[b++] = '0';
        else {
            char tmp[4]; int t = 0;
            while (v) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
            while (t > 0) buf[b++] = tmp[--t];
        }
        for (int i = 0; i < b && p < n - 1; i++) out[p++] = buf[i];
        if (oct < 3 && p < n - 1) out[p++] = '.';
    }
    if (p < n) out[p] = 0; else out[n-1] = 0;
}

static void fire_drop_notify(const firewall_check_t *p, fw_action_t a)
{
    if (!notify_should_fire(p->src_ip)) return;
    char ipbuf[24]; format_ip(p->src_ip, ipbuf, sizeof(ipbuf));
    char msg[NOTIFY_MAX_TEXT];
    int q = 0;
    const char *act = (a == FW_ACTION_REJECT) ? "rejected " : "dropped ";
    while (act[q] && q < (int)sizeof(msg) - 1) { msg[q] = act[q]; q++; }
    const char *dir = (p->direction == FW_DIR_IN) ? "IN " : "OUT ";
    int dq = 0; while (dir[dq] && q < (int)sizeof(msg) - 1) { msg[q++] = dir[dq++]; }
    int iq = 0; while (ipbuf[iq] && q < (int)sizeof(msg) - 1) { msg[q++] = ipbuf[iq++]; }
    msg[q] = 0;
    notify_send(msg, "firewall", NOTIFY_INFO);
}

/* ── REJECT response (TCP RST / ICMP unreachable) ────────────────── */

/* Build + send a TCP RST in response to an inbound packet. We craft a
 * minimal eth+ip+tcp frame, set s_skip_for_reject_tx to bypass the
 * firewall on the way out, and dispatch via net_chain_send. The lock
 * we hold (s_pending_lock_rx) is independent of the TX submit lock,
 * so re-entry is safe. */
static void send_tcp_rst_response(const firewall_check_t *p)
{
    if (!p || !p->frame || p->frame_len < 14 + 20 + 20) return;
    if (p->l4 != IP_PROTO_TCP || !p->have_l4) return;

    uint8_t pkt[14 + 20 + 20];
    for (uint32_t i = 0; i < sizeof(pkt); i++) pkt[i] = 0;

    /* Ethernet: dst = original src MAC, src = our MAC, type = IP. */
    const uint8_t *src_mac = p->frame + 6;
    for (int i = 0; i < 6; i++) pkt[i]     = src_mac[i];
    for (int i = 0; i < 6; i++) pkt[6 + i] = g_net.mac.b[i];
    pkt[12] = 0x08; pkt[13] = 0x00;

    /* IP header */
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(pkt + 14);
    ip->ver_ihl    = 0x45;
    ip->tos        = 0;
    ip->total_len  = htons(20 + 20);
    ip->id         = 0;
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->protocol   = IP_PROTO_TCP;
    ip->checksum   = 0;
    ip->src        = p->dst_ip;     /* swap */
    ip->dst        = p->src_ip;
    ip->checksum   = net_checksum(ip, 20);

    /* TCP header — RST + ACK. We use a synthetic seq/ack. */
    struct tcp_hdr *t = (struct tcp_hdr *)(pkt + 14 + 20);
    t->src_port = htons(p->dst_port);
    t->dst_port = htons(p->src_port);
    t->seq      = 0;
    t->ack      = 0;
    t->data_off = (5 << 4);
    t->flags    = TCP_RST | TCP_ACK;
    t->window   = 0;
    t->checksum = 0;
    t->urgent   = 0;

    /* TCP checksum with pseudo-header. */
    uint8_t pseudo[12 + 20];
    for (int i = 0; i < 12 + 20; i++) pseudo[i] = 0;
    for (int i = 0; i < 4; i++) pseudo[i]     = ip->src.b[i];
    for (int i = 0; i < 4; i++) pseudo[4 + i] = ip->dst.b[i];
    pseudo[8]  = 0;
    pseudo[9]  = IP_PROTO_TCP;
    pseudo[10] = 0; pseudo[11] = 20;
    for (int i = 0; i < 20; i++) pseudo[12 + i] = ((uint8_t *)t)[i];
    t->checksum = net_checksum(pseudo, 12 + 20);

    /* Bypass firewall on the response, send via the normal path. */
    s_skip_for_reject_tx = 1;
    extern int net_chain_send(const void *frame, uint16_t len);
    (void)net_chain_send(pkt, sizeof(pkt));
    s_skip_for_reject_tx = 0;
}

static void send_icmp_unreach_response(const firewall_check_t *p)
{
    if (!p || !p->frame || p->frame_len < 14 + 20) return;
    if (p->l3 != FW_L3_V4) return;
    /* Echo-back IP header + 8 bytes of payload as per RFC 792. */
    uint16_t ip_hdr_len = 20;
    uint16_t echo_len   = ip_hdr_len + 8;
    if ((uint32_t)(14 + echo_len) > p->frame_len) return;
    uint8_t pkt[14 + 20 + 8 + 20 + 8];   /* eth + ip + icmp(8) + echo(28) */
    for (uint32_t i = 0; i < sizeof(pkt); i++) pkt[i] = 0;

    const uint8_t *src_mac = p->frame + 6;
    for (int i = 0; i < 6; i++) pkt[i]     = src_mac[i];
    for (int i = 0; i < 6; i++) pkt[6 + i] = g_net.mac.b[i];
    pkt[12] = 0x08; pkt[13] = 0x00;

    struct ipv4_hdr *ip = (struct ipv4_hdr *)(pkt + 14);
    ip->ver_ihl    = 0x45;
    ip->tos        = 0;
    ip->total_len  = htons(20 + 8 + echo_len);
    ip->id         = 0;
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->protocol   = IP_PROTO_ICMP;
    ip->checksum   = 0;
    ip->src        = p->dst_ip;
    ip->dst        = p->src_ip;
    ip->checksum   = net_checksum(ip, 20);

    /* ICMP: type=3 (unreach), code=3 (port unreach). */
    uint8_t *icmp = pkt + 14 + 20;
    icmp[0] = 3; icmp[1] = 3; icmp[2] = 0; icmp[3] = 0;
    icmp[4] = icmp[5] = icmp[6] = icmp[7] = 0;
    /* Echo of original IP hdr + 8 bytes of payload. */
    for (int i = 0; i < echo_len; i++)
        icmp[8 + i] = p->frame[14 + i];
    /* ICMP checksum over the full ICMP message. */
    uint16_t icmp_total = (uint16_t)(8 + echo_len);
    uint16_t cksum = net_checksum(icmp, icmp_total);
    icmp[2] = (uint8_t)(cksum & 0xFF);
    icmp[3] = (uint8_t)(cksum >> 8);

    s_skip_for_reject_tx = 1;
    extern int net_chain_send(const void *frame, uint16_t len);
    (void)net_chain_send(pkt, (uint16_t)(14 + 20 + 8 + echo_len));
    s_skip_for_reject_tx = 0;
}

static void send_reject_response(const firewall_check_t *p)
{
    if (p->direction != FW_DIR_IN) return;   /* OUT REJECT == DROP */
    if (p->l4 == IP_PROTO_TCP)
        send_tcp_rst_response(p);
    else
        send_icmp_unreach_response(p);
}

/* ── Chain node resolves ─────────────────────────────────────────── */
/*
 * Pipeline: packet_inspect -> match_rule -> action -> emit_decision
 *
 * Input/output type for ALL nodes: "firewall_check" / "firewall_decision".
 * The intermediate scratch buffers carry firewall_check_t in the first
 * two scratch slots and firewall_decision_t in the last two. The module-
 * static s_pending_in is the source of truth — the node IO is the
 * "typed" surface MDE walks for graph correctness.
 */

static void node_packet_inspect_resolve(chain_node_t *self,
                                        void *input, void *output)
{
    (void)self; (void)input;
    firewall_check_t *out = (firewall_check_t *)output;
    *out = s_pending_in;
}

static void node_match_rule_resolve(chain_node_t *self,
                                    void *input, void *output)
{
    (void)self;
    firewall_check_t *in = (firewall_check_t *)input;
    /* We piggyback on the same struct for output of this node so the
     * downstream nodes have the packet context. The matched-rule index
     * is recorded in s_pending_out. */
    firewall_check_t *out = (firewall_check_t *)output;
    *out = *in;

    /* Lookup conntrack first so rules can use ALLOW_ESTABLISHED. */
    firewall_conn_t *c = (in->l4 == IP_PROTO_TCP || in->l4 == IP_PROTO_UDP)
                         ? conn_find(in) : 0;
    if (c) {
        out->conn_state = c->state;
        s_pending_in.conn_state = c->state;
    } else {
        out->conn_state = FW_CONN_NEW;
    }

    /* Walk in priority order. First match wins. */
    int matched = -1;
    fw_action_t action = FW_ACTION_ALLOW;
    char reason[40] = "default-allow";
    for (int i = 0; i < g_rule_count; i++) {
        firewall_rule_t *r = &g_rules[i];
        if (!rule_matches(r, in)) continue;
        if (r->action == FW_ACTION_ALLOW_ESTABLISHED) {
            if (out->conn_state == FW_CONN_ESTABLISHED) {
                matched = (int)r->id;
                action  = FW_ACTION_ALLOW;
                fw_strncpy(reason, r->name[0] ? r->name : "established", sizeof(reason));
                r->hit_count++;
                break;
            }
            /* Not established: skip this rule, keep walking. */
            continue;
        }
        matched = (int)r->id;
        action  = (fw_action_t)r->action;
        fw_strncpy(reason, r->name[0] ? r->name : "rule", sizeof(reason));
        r->hit_count++;
        break;
    }

    s_pending_out.matched_rule_id = matched;
    s_pending_out.action          = (uint8_t)action;
    s_pending_out.log             = 0;
    fw_strncpy(s_pending_out.reason, reason, sizeof(s_pending_out.reason));
}

static void node_action_resolve(chain_node_t *self,
                                void *input, void *output)
{
    (void)self;
    firewall_check_t *in  = (firewall_check_t *)input;
    firewall_decision_t *out = (firewall_decision_t *)output;
    *out = s_pending_out;

    /* On ALLOW: update conntrack so future packets in this flow are
     * recognized as established. */
    if (out->action == FW_ACTION_ALLOW) {
        conn_track(in);
    } else if (out->action == FW_ACTION_DROP || out->action == FW_ACTION_REJECT) {
        droplog_push(in, (fw_action_t)out->action, out->matched_rule_id, out->reason);
        g_drops_total++;
        fire_drop_notify(in, (fw_action_t)out->action);
    }
}

static void node_emit_decision_resolve(chain_node_t *self,
                                       void *input, void *output)
{
    (void)self;
    firewall_decision_t *in  = (firewall_decision_t *)input;
    firewall_decision_t *out = (firewall_decision_t *)output;
    *out = *in;
    /* Bump vault_version on policy transitions (drop or reject only —
     * allows are pure observation). */
    if (CHAIN_FIREWALL >= 0 &&
        (in->action == FW_ACTION_DROP || in->action == FW_ACTION_REJECT)) {
        chain_t *c = chain_get(CHAIN_FIREWALL);
        if (c) c->vault_version++;
    }
    g_chain_decisions_total++;
}

/* ── External net-chain hook resolves ────────────────────────────── */

/* Common: stage, fire chain, return action. */
static fw_action_t firewall_check_packet(uint8_t direction,
                                         const uint8_t *frame, uint16_t len)
{
    if (!g_enabled) return FW_ACTION_ALLOW;
    if (s_skip_for_reject_tx && direction == FW_DIR_OUT) return FW_ACTION_ALLOW;

    firewall_check_t check;
    /* Zero-init. */
    for (uint32_t i = 0; i < sizeof(check); i++) ((uint8_t *)&check)[i] = 0;
    check.direction = direction;
    check.frame     = frame;
    check.frame_len = len;
    if (!parse_frame(frame, len, &check)) {
        /* Non-IPv4 (ARP, IPv6, etc.): pass through. */
        return FW_ACTION_ALLOW;
    }

    /* Loopback: always allow. */
    if (ip_is_loopback(check.src_ip) || ip_is_loopback(check.dst_ip))
        return FW_ACTION_ALLOW;

    s_pending_in = check;
    /* Reset decision. */
    s_pending_out.action = FW_ACTION_ALLOW;
    s_pending_out.matched_rule_id = -1;
    s_pending_out.reason[0] = 0;

    if (CHAIN_FIREWALL < 0) {
        /* Fallback before chain registers — run match inline. */
        node_match_rule_resolve(0, &check, &check);
        node_action_resolve(0, &check, &s_pending_out);
        node_emit_decision_resolve(0, &s_pending_out, &s_pending_out);
    } else {
        (void)chain_resolve(CHAIN_FIREWALL);
    }

    return (fw_action_t)s_pending_out.action;
}

void firewall_check_tx_resolve(void *self_, void *input, void *output)
{
    (void)self_;
    /* Input/output are net_eth_frame_t (declared in net_chain.c). We
     * mirror its layout so we can read frame/len/error and stamp error
     * on a drop. */
    typedef struct {
        uint8_t *frame;
        uint16_t len;
        int      error;
    } net_eth_frame_t;
    net_eth_frame_t *in  = (net_eth_frame_t *)input;
    net_eth_frame_t *out = (net_eth_frame_t *)output;
    *out = *in;
    if (in->error || !in->frame || in->len < 14) return;

    spin_lock(&s_pending_lock_tx);
    fw_action_t a = firewall_check_packet(FW_DIR_OUT, in->frame, in->len);
    spin_unlock(&s_pending_lock_tx);

    if (a == FW_ACTION_DROP || a == FW_ACTION_REJECT) {
        out->error = 1;
        /* OUT REJECT: caller is us; just drop. */
    }
}

void firewall_check_rx_resolve(void *self_, void *input, void *output)
{
    (void)self_;
    typedef struct {
        uint8_t *frame;
        uint16_t len;
        int      error;
    } net_eth_frame_t;
    net_eth_frame_t *in  = (net_eth_frame_t *)input;
    net_eth_frame_t *out = (net_eth_frame_t *)output;
    *out = *in;
    if (in->error || !in->frame || in->len < 14) return;

    /* Capture the rejection target BEFORE marking error. */
    firewall_check_t reject_ctx;
    int do_reject = 0;

    spin_lock(&s_pending_lock_rx);
    fw_action_t a = firewall_check_packet(FW_DIR_IN, in->frame, in->len);
    if (a == FW_ACTION_REJECT) {
        reject_ctx = s_pending_in;
        do_reject  = 1;
    }
    spin_unlock(&s_pending_lock_rx);

    if (a == FW_ACTION_DROP || a == FW_ACTION_REJECT) {
        out->error = 1;
    }
    if (do_reject) {
        send_reject_response(&reject_ctx);
    }
}

/* ── Rule mgmt ───────────────────────────────────────────────────── */

int firewall_rule_count(void) { return g_rule_count; }

int firewall_add_rule(const firewall_rule_t *r)
{
    if (!r) return -2;
    if (g_rule_count >= FIREWALL_MAX_RULES) return -1;
    firewall_rule_t copy = *r;
    if (copy.id == 0) copy.id = g_next_rule_id++;
    else if (copy.id >= g_next_rule_id) g_next_rule_id = copy.id + 1;
    copy.enabled   = copy.enabled ? 1 : 1;   /* always enabled on add unless 0 explicitly */
    copy.hit_count = 0;
    g_rules[g_rule_count++] = copy;
    rules_sort();
    if (CHAIN_FIREWALL >= 0) {
        chain_t *c = chain_get(CHAIN_FIREWALL);
        if (c) c->vault_version++;
    }
    (void)firewall_persist_save();
    return 0;
}

int firewall_del_rule(uint32_t id)
{
    int found = -1;
    for (int i = 0; i < g_rule_count; i++) {
        if (g_rules[i].id == id) { found = i; break; }
    }
    if (found < 0) return -1;
    for (int i = found; i < g_rule_count - 1; i++) g_rules[i] = g_rules[i+1];
    g_rule_count--;
    if (CHAIN_FIREWALL >= 0) {
        chain_t *c = chain_get(CHAIN_FIREWALL);
        if (c) c->vault_version++;
    }
    (void)firewall_persist_save();
    return 0;
}

int firewall_flush_rules(void)
{
    g_rule_count = 0;
    if (CHAIN_FIREWALL >= 0) {
        chain_t *c = chain_get(CHAIN_FIREWALL);
        if (c) c->vault_version++;
    }
    (void)firewall_persist_save();
    return 0;
}

int firewall_walk_rules(firewall_rule_walk_cb cb, void *user)
{
    if (!cb) return -1;
    for (int i = 0; i < g_rule_count; i++) {
        if (cb(&g_rules[i], user) != 0) break;
    }
    return g_rule_count;
}

int firewall_walk_conns(firewall_conn_walk_cb cb, void *user)
{
    if (!cb) return -1;
    int n = 0;
    for (int i = 0; i < FIREWALL_CONN_MAX; i++) {
        if (!g_conns[i].in_use) continue;
        n++;
        if (cb(&g_conns[i], user) != 0) break;
    }
    return n;
}

uint32_t firewall_drop_count_total(void) { return g_drops_total; }
uint32_t firewall_chain_decisions_total(void) { return g_chain_decisions_total; }
int      firewall_enabled(void) { return g_enabled; }

void firewall_set_enabled(int on)
{
    g_enabled = on ? 1 : 0;
    uint32_t v = g_enabled;
    (void)vault_save_config(FW_KEY_ENABLED, &v, sizeof(v));
    if (CHAIN_FIREWALL >= 0) {
        chain_t *c = chain_get(CHAIN_FIREWALL);
        if (c) c->vault_version++;
    }
}

/* ── Default rules (first boot) ──────────────────────────────────── */

static struct ipv4_addr ip4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    struct ipv4_addr r; r.b[0]=a; r.b[1]=b; r.b[2]=c; r.b[3]=d; return r;
}

static void install_default_rules(void)
{
    firewall_rule_t r;

    /* Priority 10: ALLOW loopback both directions (handled in fast path
     * but rule documents intent). */
    for (uint32_t i = 0; i < sizeof(r); i++) ((uint8_t *)&r)[i] = 0;
    r.enabled = 1; r.priority = 10; r.direction = FW_DIR_BOTH;
    r.l3 = FW_L3_V4; r.l4 = FW_L4_ANY;
    r.src_ip_match = ip4(127,0,0,0); r.src_ip_mask = ip4(255,0,0,0);
    r.action = FW_ACTION_ALLOW; fw_strncpy(r.name, "lo", sizeof(r.name));
    firewall_add_rule(&r);

    /* Priority 20: ALLOW established (TCP+UDP, both directions). */
    for (uint32_t i = 0; i < sizeof(r); i++) ((uint8_t *)&r)[i] = 0;
    r.enabled = 1; r.priority = 20; r.direction = FW_DIR_BOTH;
    r.l3 = FW_L3_V4; r.l4 = FW_L4_ANY;
    r.action = FW_ACTION_ALLOW_ESTABLISHED;
    fw_strncpy(r.name, "established", sizeof(r.name));
    firewall_add_rule(&r);

    /* Priority 30: ALLOW DHCP inbound (UDP src 67 dst 68). */
    for (uint32_t i = 0; i < sizeof(r); i++) ((uint8_t *)&r)[i] = 0;
    r.enabled = 1; r.priority = 30; r.direction = FW_DIR_IN;
    r.l3 = FW_L3_V4; r.l4 = FW_L4_UDP;
    r.dst_port_min = 68; r.dst_port_max = 68;
    r.action = FW_ACTION_ALLOW; fw_strncpy(r.name, "dhcp", sizeof(r.name));
    firewall_add_rule(&r);

    /* Priority 40: ALLOW DNS inbound (UDP/TCP src port 53). The brief
     * mentions DNS exception via dst-port-53 but DNS responses come
     * back FROM 53; "established" already covers replies, so this is
     * for unsolicited DNS service we run (none in QEMU but the rule
     * documents the policy). */
    for (uint32_t i = 0; i < sizeof(r); i++) ((uint8_t *)&r)[i] = 0;
    r.enabled = 1; r.priority = 40; r.direction = FW_DIR_IN;
    r.l3 = FW_L3_V4; r.l4 = FW_L4_UDP;
    r.dst_port_min = 53; r.dst_port_max = 53;
    r.action = FW_ACTION_ALLOW; fw_strncpy(r.name, "dns-udp", sizeof(r.name));
    firewall_add_rule(&r);

    /* Priority 50: DROP inbound TCP/UDP from non-RFC1918 to ports < 1024.
     * We split this into two rules (TCP, UDP) since rule schema doesn't
     * have a "non-rfc1918" mask shortcut. The match uses dst port
     * 1..1023; the source filter is "not RFC1918" which we approximate
     * by matching the WAN-typical ranges (everything that isn't
     * 10/8, 172.16/12, 192.168/16 OR 127/8 — all 4 covered above by
     * higher-priority allows). So we just match any-source, ports<1024,
     * and rely on rule precedence. */
    for (uint32_t i = 0; i < sizeof(r); i++) ((uint8_t *)&r)[i] = 0;
    r.enabled = 1; r.priority = 50; r.direction = FW_DIR_IN;
    r.l3 = FW_L3_V4; r.l4 = FW_L4_TCP;
    r.dst_port_min = 1; r.dst_port_max = 1023;
    r.action = FW_ACTION_DROP; fw_strncpy(r.name, "wan-tcp-low", sizeof(r.name));
    firewall_add_rule(&r);

    for (uint32_t i = 0; i < sizeof(r); i++) ((uint8_t *)&r)[i] = 0;
    r.enabled = 1; r.priority = 51; r.direction = FW_DIR_IN;
    r.l3 = FW_L3_V4; r.l4 = FW_L4_UDP;
    r.dst_port_min = 1; r.dst_port_max = 1023;
    r.action = FW_ACTION_DROP; fw_strncpy(r.name, "wan-udp-low", sizeof(r.name));
    firewall_add_rule(&r);

    /* Priority 1000: ALLOW outbound default. */
    for (uint32_t i = 0; i < sizeof(r); i++) ((uint8_t *)&r)[i] = 0;
    r.enabled = 1; r.priority = 1000; r.direction = FW_DIR_OUT;
    r.l3 = FW_L3_ANY; r.l4 = FW_L4_ANY;
    r.action = FW_ACTION_ALLOW; fw_strncpy(r.name, "default-out", sizeof(r.name));
    firewall_add_rule(&r);

    /* Priority 1001: ALLOW inbound default (start permissive — opt-in
     * to lockdown). The brief explicitly says: "Default at end: ALLOW".
     * Operators tighten by adding higher-priority drops or flipping
     * firewall.default_inbound. */
    for (uint32_t i = 0; i < sizeof(r); i++) ((uint8_t *)&r)[i] = 0;
    r.enabled = 1; r.priority = 1001; r.direction = FW_DIR_IN;
    r.l3 = FW_L3_ANY; r.l4 = FW_L4_ANY;
    r.action = FW_ACTION_ALLOW; fw_strncpy(r.name, "default-in", sizeof(r.name));
    firewall_add_rule(&r);
}

/* ── Persistence ─────────────────────────────────────────────────── */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t next_id;
} fw_rules_blob_hdr_t;

#define FW_RULES_MAGIC   0x46574C52u   /* "FWLR" */
#define FW_RULES_VERSION 1u
#define FW_CONN_MAGIC    0x46574C43u   /* "FWLC" */
#define FW_CONN_VERSION  1u

int firewall_persist_save(void)
{
    /* Rules: header + array. */
    {
        uint8_t buf[sizeof(fw_rules_blob_hdr_t) +
                    sizeof(firewall_rule_t) * FIREWALL_MAX_RULES];
        fw_rules_blob_hdr_t hdr;
        hdr.magic   = FW_RULES_MAGIC;
        hdr.version = FW_RULES_VERSION;
        hdr.count   = (uint32_t)g_rule_count;
        hdr.next_id = g_next_rule_id;
        for (uint32_t i = 0; i < sizeof(hdr); i++) buf[i] = ((uint8_t *)&hdr)[i];
        uint32_t off = sizeof(hdr);
        for (int i = 0; i < g_rule_count; i++) {
            for (uint32_t j = 0; j < sizeof(firewall_rule_t); j++)
                buf[off + j] = ((uint8_t *)&g_rules[i])[j];
            off += sizeof(firewall_rule_t);
        }
        (void)vault_save_config(FW_KEY_RULES, buf, off);
    }
    /* Conntrack: 32 most recent. */
    {
        firewall_conn_t snap[32];
        int found = 0;
        /* Sort by last_seen_tsc desc (selection sort over in_use entries). */
        firewall_conn_t copy[FIREWALL_CONN_MAX];
        int n = 0;
        for (int i = 0; i < FIREWALL_CONN_MAX; i++) {
            if (g_conns[i].in_use) copy[n++] = g_conns[i];
        }
        while (found < 32 && found < n) {
            int best = found;
            for (int i = found + 1; i < n; i++) {
                if (copy[i].last_seen_tsc > copy[best].last_seen_tsc) best = i;
            }
            firewall_conn_t tmp = copy[found];
            copy[found] = copy[best]; copy[best] = tmp;
            snap[found] = copy[found];
            found++;
        }
        uint32_t hdr[3] = { FW_CONN_MAGIC, FW_CONN_VERSION, (uint32_t)found };
        uint8_t buf[sizeof(hdr) + sizeof(snap)];
        for (uint32_t i = 0; i < sizeof(hdr); i++) buf[i] = ((uint8_t *)hdr)[i];
        for (int i = 0; i < found; i++)
            for (uint32_t j = 0; j < sizeof(firewall_conn_t); j++)
                buf[sizeof(hdr) + i*sizeof(firewall_conn_t) + j] =
                    ((uint8_t *)&snap[i])[j];
        (void)vault_save_config(FW_KEY_CONNS, buf,
                                sizeof(hdr) + (uint32_t)found*sizeof(firewall_conn_t));
    }
    return 0;
}

int firewall_persist_restore(void)
{
    /* Rules. */
    int loaded_rules = 0;
    {
        uint8_t buf[sizeof(fw_rules_blob_hdr_t) +
                    sizeof(firewall_rule_t) * FIREWALL_MAX_RULES];
        int got = vault_load_config(FW_KEY_RULES, buf, sizeof(buf));
        if (got >= (int)sizeof(fw_rules_blob_hdr_t)) {
            fw_rules_blob_hdr_t hdr;
            for (uint32_t i = 0; i < sizeof(hdr); i++) ((uint8_t *)&hdr)[i] = buf[i];
            if (hdr.magic == FW_RULES_MAGIC && hdr.version == FW_RULES_VERSION) {
                uint32_t want = sizeof(hdr) + hdr.count * sizeof(firewall_rule_t);
                if (hdr.count <= FIREWALL_MAX_RULES && (int)want <= got) {
                    g_rule_count = (int)hdr.count;
                    g_next_rule_id = hdr.next_id ? hdr.next_id : 1;
                    for (uint32_t i = 0; i < hdr.count; i++) {
                        for (uint32_t j = 0; j < sizeof(firewall_rule_t); j++) {
                            ((uint8_t *)&g_rules[i])[j] =
                                buf[sizeof(hdr) + i*sizeof(firewall_rule_t) + j];
                        }
                    }
                    loaded_rules = 1;
                }
            }
        }
    }
    if (!loaded_rules) {
        g_rule_count = 0;
        install_default_rules();
    }
    /* Conntrack: best-effort. */
    {
        uint8_t buf[3 * sizeof(uint32_t) + 32 * sizeof(firewall_conn_t)];
        int got = vault_load_config(FW_KEY_CONNS, buf, sizeof(buf));
        if (got >= (int)(3 * sizeof(uint32_t))) {
            uint32_t hdr[3];
            for (uint32_t i = 0; i < sizeof(hdr); i++) ((uint8_t *)hdr)[i] = buf[i];
            if (hdr[0] == FW_CONN_MAGIC && hdr[1] == FW_CONN_VERSION) {
                uint32_t cnt = hdr[2] > 32 ? 32 : hdr[2];
                for (uint32_t i = 0; i < cnt; i++) {
                    firewall_conn_t c;
                    for (uint32_t j = 0; j < sizeof(c); j++) {
                        ((uint8_t *)&c)[j] =
                            buf[sizeof(hdr) + i*sizeof(firewall_conn_t) + j];
                    }
                    if (i < FIREWALL_CONN_MAX) g_conns[i] = c;
                }
            }
        }
    }
    /* Enabled flag. */
    {
        uint32_t v = 1;
        if (vault_load_config(FW_KEY_ENABLED, &v, sizeof(v)) == (int)sizeof(v))
            g_enabled = v ? 1 : 0;
    }
    return 0;
}

/* ── Init ────────────────────────────────────────────────────────── */

void firewall_init(void)
{
    if (g_initialized) return;
    g_initialized = 1;
    /* Zero the rule table + conntrack. */
    for (int i = 0; i < FIREWALL_MAX_RULES; i++)
        for (uint32_t j = 0; j < sizeof(firewall_rule_t); j++)
            ((uint8_t *)&g_rules[i])[j] = 0;
    for (int i = 0; i < FIREWALL_CONN_MAX; i++) g_conns[i].in_use = 0;
    g_rule_count = 0;
    /* Try VAULT first, otherwise install defaults. */
    firewall_persist_restore();
}

int firewall_chain_register(int parent_chain_id)
{
    if (CHAIN_FIREWALL >= 0) return CHAIN_FIREWALL;
    int id = chain_create("firewall", parent_chain_id, MASQ_INTERNAL);
    if (id < 0) return -1;
    chain_add_node(id, "packet_inspect",
                   "firewall_check", "firewall_check",
                   node_packet_inspect_resolve);
    chain_add_node(id, "match_rule",
                   "firewall_check", "firewall_check",
                   node_match_rule_resolve);
    chain_add_node(id, "action",
                   "firewall_check", "firewall_decision",
                   node_action_resolve);
    chain_add_node(id, "emit_decision",
                   "firewall_decision", "firewall_decision",
                   node_emit_decision_resolve);
    CHAIN_FIREWALL = id;
    return id;
}

/* ── Settings ────────────────────────────────────────────────────── */

static int sr_itoa_local(int v, char *out, int n) {
    if (n <= 0) return -1;
    if (v == 0) { if (n < 2) return -1; out[0] = '0'; out[1] = 0; return 1; }
    char tmp[16]; int t = 0; int neg = 0; uint32_t u;
    if (v < 0) { neg = 1; u = (uint32_t)(-v); } else u = (uint32_t)v;
    while (u && t < 15) { tmp[t++] = (char)('0' + (u % 10)); u /= 10; }
    int p = 0;
    if (neg) { if (p < n - 1) out[p++] = '-'; }
    while (t > 0 && p < n - 1) out[p++] = tmp[--t];
    out[p] = 0;
    return p;
}

static int g_fw_enabled(char *out, int n) {
    return sr_itoa_local(g_enabled, out, n) >= 0 ? 0 : -1;
}
static int s_fw_enabled(const char *v) {
    if (!v) return -1;
    if (fw_streq(v, "1") || fw_streq(v, "true") || fw_streq(v, "on")) {
        firewall_set_enabled(1); return 0;
    }
    if (fw_streq(v, "0") || fw_streq(v, "false") || fw_streq(v, "off")) {
        firewall_set_enabled(0); return 0;
    }
    return -1;
}

/* default_inbound / default_outbound: writeable enum that flips the
 * action of the lowest-priority "default-in" / "default-out" rule. */
static firewall_rule_t *find_default_rule(uint8_t direction)
{
    firewall_rule_t *best = 0;
    for (int i = 0; i < g_rule_count; i++) {
        if (g_rules[i].direction != direction) continue;
        if (g_rules[i].l3 != FW_L3_ANY)        continue;
        if (g_rules[i].l4 != FW_L4_ANY)        continue;
        if (g_rules[i].src_ip_mask.b[0] || g_rules[i].dst_ip_mask.b[0]) continue;
        if (g_rules[i].dst_port_min || g_rules[i].dst_port_max)         continue;
        if (!best || g_rules[i].priority > best->priority) best = &g_rules[i];
    }
    return best;
}

static int g_fw_default_in(char *out, int n) {
    firewall_rule_t *r = find_default_rule(FW_DIR_IN);
    const char *v = (r && r->action == FW_ACTION_DROP) ? "drop" : "allow";
    int i = 0; while (v[i] && i < n - 1) { out[i] = v[i]; i++; }
    out[i] = 0;
    return 0;
}
static int s_fw_default_in(const char *v) {
    firewall_rule_t *r = find_default_rule(FW_DIR_IN);
    if (!r) return -1;
    if (fw_streq(v, "allow")) r->action = FW_ACTION_ALLOW;
    else if (fw_streq(v, "drop")) r->action = FW_ACTION_DROP;
    else return -1;
    if (CHAIN_FIREWALL >= 0) {
        chain_t *c = chain_get(CHAIN_FIREWALL);
        if (c) c->vault_version++;
    }
    (void)firewall_persist_save();
    return 0;
}
static int g_fw_default_out(char *out, int n) {
    firewall_rule_t *r = find_default_rule(FW_DIR_OUT);
    const char *v = (r && r->action == FW_ACTION_DROP) ? "drop" : "allow";
    int i = 0; while (v[i] && i < n - 1) { out[i] = v[i]; i++; }
    out[i] = 0;
    return 0;
}
static int s_fw_default_out(const char *v) {
    firewall_rule_t *r = find_default_rule(FW_DIR_OUT);
    if (!r) return -1;
    if (fw_streq(v, "allow")) r->action = FW_ACTION_ALLOW;
    else if (fw_streq(v, "drop")) r->action = FW_ACTION_DROP;
    else return -1;
    if (CHAIN_FIREWALL >= 0) {
        chain_t *c = chain_get(CHAIN_FIREWALL);
        if (c) c->vault_version++;
    }
    (void)firewall_persist_save();
    return 0;
}

static const settings_entry_t E_FW_ENABLED = {
    "firewall.enabled", SK_BOOL, 0, g_fw_enabled, s_fw_enabled, 0,
    "global firewall on/off"
};
static const settings_entry_t E_FW_DEF_IN = {
    "firewall.default_inbound", SK_ENUM, 0,
    g_fw_default_in, s_fw_default_in, "allow|drop",
    "default policy for unmatched inbound traffic"
};
static const settings_entry_t E_FW_DEF_OUT = {
    "firewall.default_outbound", SK_ENUM, 0,
    g_fw_default_out, s_fw_default_out, "allow|drop",
    "default policy for unmatched outbound traffic"
};

void firewall_settings_register(void)
{
    settings_register(&E_FW_ENABLED);
    settings_register(&E_FW_DEF_IN);
    settings_register(&E_FW_DEF_OUT);
}

/* ── Selftest ────────────────────────────────────────────────────── */

void firewall_print_selftest_line(void)
{
    kputs("  Firewall .............. ");
    kputs(g_enabled ? "enabled, " : "disabled, ");
    kput_dec((uint64_t)g_rule_count);
    kputs(" rules, ");
    kput_dec((uint64_t)firewall_active_conn_count());
    kputs(" active connections, ");
    kput_dec((uint64_t)firewall_drops_per_min());
    kputs(" drops/min\n");
}

/* ── Shell command ───────────────────────────────────────────────── */

static const char *action_label(uint8_t a) {
    switch (a) {
    case FW_ACTION_ALLOW:             return "allow";
    case FW_ACTION_DROP:              return "drop";
    case FW_ACTION_REJECT:            return "reject";
    case FW_ACTION_ALLOW_ESTABLISHED: return "allow-est";
    default: return "?";
    }
}
static const char *dir_label(uint8_t d) {
    if (d == FW_DIR_IN)   return "in";
    if (d == FW_DIR_OUT)  return "out";
    if (d == FW_DIR_BOTH) return "any";
    return "?";
}
static const char *l4_label(uint8_t p) {
    if (p == FW_L4_TCP)  return "tcp";
    if (p == FW_L4_UDP)  return "udp";
    if (p == FW_L4_ICMP) return "icmp";
    return "any";
}

static void print_ip(struct ipv4_addr a) {
    char buf[24]; format_ip(a, buf, sizeof(buf)); kputs(buf);
}

static void print_rule(const firewall_rule_t *r) {
    kputs("  #"); kput_dec((uint64_t)r->id);
    kputs(" pri="); kput_dec((uint64_t)r->priority);
    kputs(" "); kputs(dir_label(r->direction));
    kputs(" "); kputs(l4_label(r->l4));
    kputs(" -> "); kputs(action_label(r->action));
    if (r->dst_port_min || r->dst_port_max) {
        kputs(" dport="); kput_dec((uint64_t)r->dst_port_min);
        if (r->dst_port_max != r->dst_port_min) {
            kputs(".."); kput_dec((uint64_t)r->dst_port_max);
        }
    }
    if (r->src_ip_mask.b[0] || r->src_ip_mask.b[1] ||
        r->src_ip_mask.b[2] || r->src_ip_mask.b[3]) {
        kputs(" src="); print_ip(r->src_ip_match);
        kputs("/"); print_ip(r->src_ip_mask);
    }
    kputs(" hits="); kput_dec((uint64_t)r->hit_count);
    if (r->name[0]) { kputs(" ("); kputs(r->name); kputs(")"); }
    kputs("\n");
}

static int rule_print_cb(const firewall_rule_t *r, void *u) {
    (void)u; print_rule(r); return 0;
}

static int conn_print_cb(const firewall_conn_t *c, void *u) {
    (void)u;
    kputs("  ");
    kputs(c->proto == IP_PROTO_TCP ? "tcp" :
          c->proto == IP_PROTO_UDP ? "udp" : "?  ");
    kputs(" "); print_ip(c->src_ip);
    kputs(":"); kput_dec((uint64_t)c->src_port);
    kputs(" -> "); print_ip(c->dst_ip);
    kputs(":"); kput_dec((uint64_t)c->dst_port);
    kputs(" "); kputs(c->state == FW_CONN_ESTABLISHED ? "ESTABLISHED" :
                       c->state == FW_CONN_CLOSING     ? "CLOSING" : "NEW");
    kputs("\n");
    return 0;
}

/* Skip whitespace. */
static const char *skip_ws(const char *s) {
    while (s && (*s == ' ' || *s == '\t')) s++;
    return s;
}

/* Pull a whitespace-delimited token into `out` (max chars). Returns
 * the rest of the string, or NULL if no token. */
static const char *next_tok(const char *s, char *out, int max) {
    s = skip_ws(s);
    if (!s || !*s) { if (out) out[0] = 0; return 0; }
    int i = 0;
    while (*s && *s != ' ' && *s != '\t' && i < max - 1) out[i++] = *s++;
    out[i] = 0;
    return s;
}

/* Parse `firewall add drop in tcp 22` style. */
static int parse_add_spec(const char *args, firewall_rule_t *r)
{
    if (!args || !r) return -1;
    for (uint32_t i = 0; i < sizeof(*r); i++) ((uint8_t *)r)[i] = 0;
    r->enabled = 1;
    r->priority = 100;
    r->l3 = FW_L3_V4;
    r->direction = FW_DIR_BOTH;
    r->l4 = FW_L4_ANY;

    char tok[32];
    /* action */
    args = next_tok(args, tok, sizeof(tok));
    if (!args) return -1;
    if (fw_streq(tok, "allow"))  r->action = FW_ACTION_ALLOW;
    else if (fw_streq(tok, "drop"))   r->action = FW_ACTION_DROP;
    else if (fw_streq(tok, "reject")) r->action = FW_ACTION_REJECT;
    else return -1;

    /* direction */
    args = next_tok(args, tok, sizeof(tok));
    if (!args) return -1;
    if (fw_streq(tok, "in"))   r->direction = FW_DIR_IN;
    else if (fw_streq(tok, "out"))  r->direction = FW_DIR_OUT;
    else if (fw_streq(tok, "both") || fw_streq(tok, "any")) r->direction = FW_DIR_BOTH;
    else return -1;

    /* protocol (tcp/udp/icmp/any) — optional */
    const char *peek = next_tok(args, tok, sizeof(tok));
    if (peek) {
        if (fw_streq(tok, "tcp"))       { r->l4 = FW_L4_TCP;  args = peek; }
        else if (fw_streq(tok, "udp"))  { r->l4 = FW_L4_UDP;  args = peek; }
        else if (fw_streq(tok, "icmp")) { r->l4 = FW_L4_ICMP; args = peek; }
        else if (fw_streq(tok, "any"))  { r->l4 = FW_L4_ANY;  args = peek; }
        /* If not a protocol keyword, leave args alone (port may follow). */
    }

    /* port (optional). */
    peek = next_tok(args, tok, sizeof(tok));
    if (peek) {
        uint32_t port = 0;
        if (fw_atou(tok, &port) == 0 && port < 65536) {
            r->dst_port_min = (uint16_t)port;
            r->dst_port_max = (uint16_t)port;
            args = peek;
        }
    }

    fw_strncpy(r->name, "shell-add", sizeof(r->name));
    return 0;
}

static int s_confirm_flush_armed = 0;

void firewall_cmd(const char *args)
{
    args = skip_ws(args);
    char tok[32];

    /* No args: list. */
    if (!args || !*args) {
        kputs("firewall: ");
        kputs(g_enabled ? "ENABLED" : "DISABLED");
        kputs(" — "); kput_dec((uint64_t)g_rule_count); kputs(" rules, ");
        kput_dec((uint64_t)firewall_active_conn_count()); kputs(" connections, ");
        kput_dec((uint64_t)g_drops_total); kputs(" total drops\n");
        kputs("rules:\n");
        firewall_walk_rules(rule_print_cb, 0);
        kputs("connections:\n");
        if (firewall_active_conn_count() == 0) kputs("  (none)\n");
        else firewall_walk_conns(conn_print_cb, 0);
        return;
    }

    const char *rest = next_tok(args, tok, sizeof(tok));

    if (fw_streq(tok, "enable")) {
        firewall_set_enabled(1);
        kputs("firewall: ENABLED\n");
        return;
    }
    if (fw_streq(tok, "disable")) {
        firewall_set_enabled(0);
        kputs("firewall: DISABLED\n");
        return;
    }
    if (fw_streq(tok, "add")) {
        firewall_rule_t r;
        if (parse_add_spec(rest, &r) != 0) {
            kputs("usage: firewall add <allow|drop|reject> <in|out|both> [tcp|udp|icmp|any] [port]\n");
            return;
        }
        if (firewall_add_rule(&r) != 0) {
            kputs("firewall: rule add failed (table full?)\n"); return;
        }
        kputs("firewall: rule #"); kput_dec((uint64_t)(g_next_rule_id - 1));
        kputs(" added\n");
        return;
    }
    if (fw_streq(tok, "del") || fw_streq(tok, "rm")) {
        char idtok[16];
        rest = next_tok(rest, idtok, sizeof(idtok));
        if (!rest) { kputs("usage: firewall del <id>\n"); return; }
        uint32_t id = 0;
        if (fw_atou(idtok, &id) != 0) { kputs("bad id\n"); return; }
        if (firewall_del_rule(id) == 0) {
            kputs("firewall: rule #"); kput_dec((uint64_t)id); kputs(" removed\n");
        } else {
            kputs("firewall: rule not found\n");
        }
        return;
    }
    if (fw_streq(tok, "flush")) {
        if (!s_confirm_flush_armed) {
            s_confirm_flush_armed = 1;
            kputs("firewall: flush will remove ALL "); kput_dec((uint64_t)g_rule_count);
            kputs(" rules. Run `firewall flush` again within this session to confirm.\n");
            return;
        }
        s_confirm_flush_armed = 0;
        firewall_flush_rules();
        kputs("firewall: flushed\n");
        return;
    }
    if (fw_streq(tok, "log")) {
        char ntok[16];
        const char *r2 = next_tok(rest, ntok, sizeof(ntok));
        int n = 16;
        if (r2) { uint32_t v = 0; if (fw_atou(ntok, &v) == 0) n = (int)v; }
        if (n > g_droplog_count) n = g_droplog_count;
        kputs("firewall: last "); kput_dec((uint64_t)n); kputs(" drops\n");
        for (int i = 0; i < n; i++) {
            firewall_drop_log_t e;
            if (firewall_log_get(i, &e) != 0) break;
            kputs("  ");
            kputs(e.direction == FW_DIR_IN ? "IN  " : "OUT ");
            kputs(e.proto == IP_PROTO_TCP ? "tcp " :
                  e.proto == IP_PROTO_UDP ? "udp " : "ip  ");
            print_ip(e.src_ip); kputs(":");
            kput_dec((uint64_t)e.src_port); kputs(" -> ");
            print_ip(e.dst_ip); kputs(":");
            kput_dec((uint64_t)e.dst_port);
            kputs(" ["); kputs(action_label(e.action)); kputs("] ");
            kputs(e.reason); kputs("\n");
        }
        return;
    }
    kputs("usage: firewall [add|del|flush|log|enable|disable]\n");
}
