/*
 * Zeos -- TCP over IPv6 (parallel slot pool).
 *
 * Mirrors net_tcp.c's state machine but on its own pool and dispatching
 * through ipv6_send() / IPV6_NH_TCP. v4 and v6 connections live in
 * separate pools so neither file touches the other.
 */

#include "net_tcp6.h"
#include "net_ipv6.h"
#include "net.h"
#include "timer.h"
#include "kprint.h"

/* ── Slot pool ─────────────────────────────────── */

#define TCP6_MAX_CONNECTIONS  4
#define TCP6_RX_BUF_SIZE      8192
#define TCP6_RETX_BUF_SIZE    1300
#define TCP6_RETX_MAX         3

struct tcp6_conn {
    struct ipv6_addr remote_ip;
    uint16_t         local_port;
    uint16_t         remote_port;
    uint32_t         seq;
    uint32_t         ack;
    enum tcp_state   state;
    int              in_use;

    uint8_t          rx_buf[TCP6_RX_BUF_SIZE];
    uint16_t         rx_len;
    uint16_t         rx_read;
    int              remote_closed;

    uint8_t          retx_buf[TCP6_RETX_BUF_SIZE];
    uint16_t         retx_len;
    uint32_t         retx_seq;
    uint8_t          retx_flags;
    uint8_t          retx_count;
    uint64_t         retx_tsc;
    uint32_t         retx_expected_ack;
};

static struct tcp6_conn s_pool[TCP6_MAX_CONNECTIONS];

static uint64_t rdtsc6_tcp(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t retx_timeout(void) {
    uint64_t f = timer_tsc_freq();
    return f ? f : 3000000000ULL;
}

static struct tcp6_conn *get(tcp_handle_t h) {
    if (h < 0 || h >= TCP6_MAX_CONNECTIONS) return 0;
    if (!s_pool[h].in_use) return 0;
    return &s_pool[h];
}

static struct tcp6_conn *find(struct ipv6_addr src, uint16_t sport, uint16_t dport)
{
    for (int i = 0; i < TCP6_MAX_CONNECTIONS; i++) {
        struct tcp6_conn *c = &s_pool[i];
        if (!c->in_use) continue;
        if (c->local_port == dport && c->remote_port == sport && ipv6_eq(c->remote_ip, src))
            return c;
    }
    return 0;
}

/* ── Pseudo-header checksum (TCP/IPv6) ────────── */

static uint16_t tcp6_checksum(struct ipv6_addr src, struct ipv6_addr dst,
                              const void *tcp_data, uint16_t tcp_len)
{
    uint32_t sum = 0;
    const uint16_t *p = (const uint16_t *)src.bytes;
    for (int i = 0; i < 8; i++) sum += p[i];
    p = (const uint16_t *)dst.bytes;
    for (int i = 0; i < 8; i++) sum += p[i];
    uint32_t lh = htonl((uint32_t)tcp_len);
    sum += (lh & 0xFFFF);
    sum += (lh >> 16);
    sum += htons((uint16_t)IPV6_NH_TCP);
    p = (const uint16_t *)tcp_data;
    int rem = tcp_len;
    while (rem > 1) { sum += *p++; rem -= 2; }
    if (rem == 1) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ── Send a segment ────────────────────────────── */

static int send_seg(struct tcp6_conn *c, uint8_t flags, const void *data, uint16_t dlen)
{
    uint8_t pkt[1280];
    if (dlen + sizeof(struct tcp_hdr) > sizeof(pkt)) return -1;
    struct tcp_hdr *t = (struct tcp_hdr *)pkt;
    t->src_port = htons(c->local_port);
    t->dst_port = htons(c->remote_port);
    t->seq = htonl(c->seq);
    t->ack = htonl(c->ack);
    t->data_off = 0x50;
    t->flags = flags;
    t->window = htons(4096);
    t->checksum = 0;
    t->urgent = 0;
    if (data && dlen) {
        const uint8_t *s = data;
        for (uint16_t i = 0; i < dlen; i++) pkt[sizeof(struct tcp_hdr) + i] = s[i];
    }
    uint16_t total = (uint16_t)(sizeof(struct tcp_hdr) + dlen);
    struct ipv6_addr src6 = ipv6_pick_source(c->remote_ip);
    t->checksum = tcp6_checksum(src6, c->remote_ip, pkt, total);
    return ipv6_send(c->remote_ip, IPV6_NH_TCP, pkt, total);
}

static int send_tracked(struct tcp6_conn *c, uint8_t flags, const void *data,
                         uint16_t dlen, uint32_t expected_ack)
{
    int rc = send_seg(c, flags, data, dlen);
    if (dlen <= TCP6_RETX_BUF_SIZE) {
        if (data && dlen) {
            const uint8_t *s = data;
            for (uint16_t i = 0; i < dlen; i++) c->retx_buf[i] = s[i];
        }
        c->retx_len = dlen;
        c->retx_seq = c->seq;
        c->retx_flags = flags;
        c->retx_count = 0;
        c->retx_tsc = rdtsc6_tcp();
        c->retx_expected_ack = expected_ack;
    }
    return rc;
}

static void retx_clear(struct tcp6_conn *c) {
    c->retx_len = 0; c->retx_count = 0; c->retx_tsc = 0; c->retx_expected_ack = 0;
}

/* ── Public API ───────────────────────────────── */

tcp_handle_t tcp_open_v6(struct ipv6_addr dst, uint16_t port)
{
    int slot = -1;
    for (int i = 0; i < TCP6_MAX_CONNECTIONS; i++) {
        if (!s_pool[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return TCP_INVALID_HANDLE;
    struct tcp6_conn *c = &s_pool[slot];
    c->remote_ip = dst;
    c->remote_port = port;
    c->local_port = 49152 + (uint16_t)(rdtsc6_tcp() & 0x3FFF);
    c->seq = (uint32_t)rdtsc6_tcp();
    c->ack = 0;
    c->state = TCP_CLOSED;
    c->in_use = 1;
    c->rx_len = 0; c->rx_read = 0; c->remote_closed = 0;
    retx_clear(c);

    c->state = TCP_SYN_SENT;
    uint32_t syn_expect = c->seq + 1;
    send_tracked(c, TCP_SYN, 0, 0, syn_expect);
    c->seq++;

    for (int i = 0; i < 50000 && c->state == TCP_SYN_SENT; i++) {
        net_poll();
        tcp6_retransmit_tick();
        for (volatile int j = 0; j < 5000; j++);
        if (c->state == TCP_ERROR) break;
    }
    if (c->state != TCP_ESTABLISHED) {
        c->state = TCP_CLOSED;
        c->in_use = 0;
        return TCP_INVALID_HANDLE;
    }
    return slot;
}

int tcp_send_v6(tcp_handle_t h, const void *data, uint16_t len)
{
    struct tcp6_conn *c = get(h);
    if (!c || c->state != TCP_ESTABLISHED) return -1;
    uint16_t mss = 1220;
    uint16_t sent = 0;
    while (sent < len) {
        uint16_t chunk = len - sent;
        if (chunk > mss) chunk = mss;
        uint32_t expect = c->seq + chunk;
        send_tracked(c, TCP_ACK | TCP_PSH, (const uint8_t *)data + sent, chunk, expect);
        c->seq += chunk;
        sent += chunk;
        for (int i = 0; i < 50000; i++) {
            net_poll();
            tcp6_retransmit_tick();
            if (c->retx_len == 0) break;
            if (c->state == TCP_ERROR) return -1;
            for (volatile int j = 0; j < 1000; j++);
        }
        if (c->retx_len) { c->state = TCP_ERROR; return -1; }
    }
    return sent;
}

int tcp_recv_v6(tcp_handle_t h, void *buf, uint16_t max_len)
{
    struct tcp6_conn *c = get(h);
    if (!c) return -1;
    if (c->rx_read < c->rx_len) {
        uint16_t avail = c->rx_len - c->rx_read;
        uint16_t n = avail > max_len ? max_len : avail;
        uint8_t *d = buf;
        for (uint16_t i = 0; i < n; i++) d[i] = c->rx_buf[c->rx_read + i];
        c->rx_read += n;
        return n;
    }
    c->rx_len = 0; c->rx_read = 0;
    for (int i = 0; i < 50000; i++) {
        net_poll();
        tcp6_retransmit_tick();
        if (c->rx_len > 0) {
            uint16_t n = c->rx_len > max_len ? max_len : c->rx_len;
            uint8_t *d = buf;
            for (uint16_t j = 0; j < n; j++) d[j] = c->rx_buf[j];
            c->rx_read = n;
            return n;
        }
        if (c->remote_closed || c->state == TCP_CLOSED || c->state == TCP_ERROR)
            return 0;
        for (volatile int j = 0; j < 1000; j++);
    }
    return 0;
}

void tcp_close_v6(tcp_handle_t h)
{
    struct tcp6_conn *c = get(h);
    if (!c) return;
    if (c->state == TCP_ESTABLISHED) {
        c->state = TCP_FIN_WAIT_1;
        uint32_t expect = c->seq + 1;
        send_tracked(c, TCP_FIN | TCP_ACK, 0, 0, expect);
        c->seq++;
        for (int i = 0; i < 20000 && c->state != TCP_CLOSED; i++) {
            net_poll();
            tcp6_retransmit_tick();
            if (c->state == TCP_ERROR) break;
            for (volatile int j = 0; j < 1000; j++);
        }
    }
    c->state = TCP_CLOSED;
    c->in_use = 0;
    retx_clear(c);
}

void tcp6_retransmit_tick(void)
{
    uint64_t now = rdtsc6_tcp();
    uint64_t to = retx_timeout();
    for (int i = 0; i < TCP6_MAX_CONNECTIONS; i++) {
        struct tcp6_conn *c = &s_pool[i];
        if (!c->in_use || c->retx_len == 0) continue;
        if (c->state == TCP_CLOSED || c->state == TCP_ERROR) continue;
        if ((now - c->retx_tsc) < to) continue;
        c->retx_count++;
        if (c->retx_count > TCP6_RETX_MAX) {
            c->state = TCP_ERROR;
            retx_clear(c);
            continue;
        }
        uint32_t saved = c->seq;
        c->seq = c->retx_seq;
        send_seg(c, c->retx_flags, c->retx_len ? c->retx_buf : 0, c->retx_len);
        c->seq = saved;
        c->retx_tsc = now;
    }
}

void tcp6_process(const struct ipv6_addr *src, const struct ipv6_addr *dst,
                  const void *tcp_pkt, uint16_t tcp_len)
{
    (void)dst;
    if (tcp_len < sizeof(struct tcp_hdr)) return;
    const struct tcp_hdr *t = (const struct tcp_hdr *)tcp_pkt;
    uint16_t sport = ntohs(t->src_port);
    uint16_t dport = ntohs(t->dst_port);
    struct tcp6_conn *c = find(*src, sport, dport);
    if (!c) return;

    uint32_t seg_seq = ntohl(t->seq);
    uint32_t seg_ack = ntohl(t->ack);
    uint8_t flags = t->flags;
    uint16_t hlen = ((t->data_off >> 4) & 0x0F) * 4;
    if (hlen > tcp_len) return;
    uint16_t dlen = (uint16_t)(tcp_len - hlen);
    const uint8_t *data = (const uint8_t *)t + hlen;

    if (flags & TCP_RST) {
        c->state = TCP_CLOSED;
        c->remote_closed = 1;
        retx_clear(c);
        return;
    }
    if ((flags & TCP_ACK) && c->retx_len > 0) {
        if (seg_ack >= c->retx_expected_ack) retx_clear(c);
    }
    if (c->state == TCP_SYN_SENT && c->retx_expected_ack &&
        (flags & (TCP_SYN|TCP_ACK)) == (TCP_SYN|TCP_ACK)) {
        if (seg_ack >= c->retx_expected_ack) retx_clear(c);
    }

    switch (c->state) {
    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN|TCP_ACK)) == (TCP_SYN|TCP_ACK)) {
            c->ack = seg_seq + 1;
            c->state = TCP_ESTABLISHED;
            retx_clear(c);
            send_seg(c, TCP_ACK, 0, 0);
        }
        break;
    case TCP_ESTABLISHED:
        if (dlen > 0) {
            uint16_t space = TCP6_RX_BUF_SIZE - c->rx_len;
            uint16_t n = dlen > space ? space : dlen;
            for (uint16_t i = 0; i < n; i++) c->rx_buf[c->rx_len + i] = data[i];
            c->rx_len += n;
            c->ack = seg_seq + dlen;
            send_seg(c, TCP_ACK, 0, 0);
        }
        if (flags & TCP_FIN) {
            c->ack = seg_seq + dlen + 1;
            c->remote_closed = 1;
            send_seg(c, TCP_ACK, 0, 0);
            send_seg(c, TCP_FIN|TCP_ACK, 0, 0);
            c->seq++;
            c->state = TCP_LAST_ACK;
        }
        break;
    case TCP_FIN_WAIT_1:
        if (flags & TCP_ACK) {
            retx_clear(c);
            if (flags & TCP_FIN) {
                c->ack = seg_seq + 1;
                send_seg(c, TCP_ACK, 0, 0);
                c->state = TCP_CLOSED;
                c->in_use = 0;
            } else c->state = TCP_FIN_WAIT_2;
        }
        break;
    case TCP_FIN_WAIT_2:
        if (flags & TCP_FIN) {
            c->ack = seg_seq + 1;
            send_seg(c, TCP_ACK, 0, 0);
            c->state = TCP_CLOSED;
            c->in_use = 0;
        }
        break;
    case TCP_LAST_ACK:
        if (flags & TCP_ACK) {
            c->state = TCP_CLOSED;
            c->in_use = 0;
            retx_clear(c);
        }
        break;
    default:
        break;
    }
    (void)seg_ack;
}
