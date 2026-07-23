/*
 * Zeos — TCP implementation (multi-connection with retransmission)
 *
 * Connection pool of TCP_MAX_CONNECTIONS slots.
 * Incoming packets demuxed by (src_ip, src_port, dst_port).
 * Retransmission: unacked segments retransmit at ~1s intervals, up to 3 times.
 * Legacy single-connection API preserved via slot 0 wrappers.
 */

#include "net_tcp.h"
#include "net_ip.h"
#include "chain.h"
#include "chain_registry.h"
#include "timer.h"
#include "kprint.h"

/* ── Connection pool ─────────────────────────── */

static struct tcp_conn connections[TCP_MAX_CONNECTIONS];

/* ── Listener table ──────────────────────────── */

struct tcp_listener {
    int              in_use;
    uint16_t         port;
    tcp_accept_cb_t  accept_cb;
    int              pending[TCP_LISTEN_BACKLOG]; /* connection slot indices, -1 = empty */
    int              pending_count;
    int              accepted_total;             /* lifetime count for selftest */
};

static struct tcp_listener listeners[TCP_MAX_LISTENERS];

static struct tcp_listener *listener_find(uint16_t port)
{
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (listeners[i].in_use && listeners[i].port == port)
            return &listeners[i];
    }
    return 0;
}

static int listener_idx_for_port(uint16_t port)
{
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (listeners[i].in_use && listeners[i].port == port)
            return i;
    }
    return -1;
}

static void listener_push(struct tcp_listener *L, int slot)
{
    for (int i = 0; i < TCP_LISTEN_BACKLOG; i++) {
        if (L->pending[i] < 0) {
            L->pending[i] = slot;
            L->pending_count++;
            return;
        }
    }
    /* Backlog full — silently drop. The half-open conn will time out
     * via the SYN retransmit chain on the client side. */
}

static int listener_pop(struct tcp_listener *L) __attribute__((unused));
static int listener_pop(struct tcp_listener *L)
{
    for (int i = 0; i < TCP_LISTEN_BACKLOG; i++) {
        if (L->pending[i] >= 0) {
            int s = L->pending[i];
            L->pending[i] = -1;
            L->pending_count--;
            return s;
        }
    }
    return -1;
}

/* ── Internal helpers ────────────────────────── */

static uint64_t read_tsc_tcp(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Approximate 1 second in TSC ticks. Falls back to ~3 GHz estimate
   if timer_tsc_freq() returns 0 (calibration not done yet). */
static uint64_t retx_timeout_tsc(void)
{
    uint64_t freq = timer_tsc_freq();
    if (freq == 0)
        freq = 3000000000ULL;  /* Conservative 3 GHz fallback */
    return freq;               /* 1 second worth of TSC ticks */
}

/* Allocate a free connection slot. Returns index or -1. */
static int conn_alloc(void)
{
    /* Slot 0 is reserved for legacy API — new API starts at 1 */
    for (int i = 1; i < TCP_MAX_CONNECTIONS; i++) {
        if (!connections[i].in_use && connections[i].state == TCP_CLOSED)
            return i;
    }
    return -1;
}

/* Get connection pointer from handle, with validation. */
static struct tcp_conn *conn_get(tcp_handle_t h)
{
    if (h < 0 || h >= TCP_MAX_CONNECTIONS)
        return 0;
    if (!connections[h].in_use)
        return 0;
    return &connections[h];
}

/* Find connection matching an incoming packet's tuple. */
static struct tcp_conn *conn_find(struct ipv4_addr src_ip,
                                   uint16_t src_port, uint16_t dst_port)
{
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        struct tcp_conn *c = &connections[i];
        if (!c->in_use)
            continue;
        if (c->local_port == dst_port &&
            c->remote_port == src_port &&
            ip_eq(c->remote_ip, src_ip))
            return c;
    }
    return 0;
}

/* ── TCP pseudo-header checksum ──────────────── */

struct tcp_pseudo {
    struct ipv4_addr src;
    struct ipv4_addr dst;
    uint8_t          zero;
    uint8_t          protocol;
    uint16_t         tcp_len;
};

static uint16_t tcp_checksum(struct ipv4_addr src, struct ipv4_addr dst,
                              const void *tcp_data, uint16_t tcp_len)
{
    struct tcp_pseudo pseudo;
    pseudo.src = src;
    pseudo.dst = dst;
    pseudo.zero = 0;
    pseudo.protocol = IP_PROTO_TCP;
    pseudo.tcp_len = htons(tcp_len);

    /* Sum pseudo-header */
    uint32_t sum = 0;
    const uint16_t *p = (const uint16_t *)&pseudo;
    for (int i = 0; i < 6; i++)
        sum += p[i];

    /* Sum TCP data */
    p = (const uint16_t *)tcp_data;
    int remaining = tcp_len;
    while (remaining > 1) {
        sum += *p++;
        remaining -= 2;
    }
    if (remaining == 1)
        sum += *(const uint8_t *)p;

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}

/* ── Send a TCP segment ──────────────────────── */

static int tcp_send_segment(struct tcp_conn *conn, uint8_t flags,
                             const void *data, uint16_t data_len)
{
    uint8_t pkt[NET_MTU];
    struct tcp_hdr *tcp = (struct tcp_hdr *)pkt;

    tcp->src_port = htons(conn->local_port);
    tcp->dst_port = htons(conn->remote_port);
    tcp->seq = htonl(conn->seq);
    tcp->ack = htonl(conn->ack);
    tcp->data_off = 0x50;   /* 5 dwords = 20 bytes, no options */
    tcp->flags = flags;
    /* Advertise the TRUE free receive space, not a fixed 4096 that lies once the
     * buffer fills (the peer would keep sending, we'd drop + our ACK-only-buffered
     * logic would leave a hole). Real flow control. */
    tcp->window = htons(conn->rx_len < TCP_RX_BUF_SIZE
                        ? (uint16_t)(TCP_RX_BUF_SIZE - conn->rx_len) : 0);
    tcp->checksum = 0;
    tcp->urgent = 0;

    /* Copy payload */
    if (data && data_len > 0) {
        const uint8_t *src = (const uint8_t *)data;
        for (uint16_t i = 0; i < data_len; i++)
            pkt[sizeof(struct tcp_hdr) + i] = src[i];
    }

    uint16_t total = sizeof(struct tcp_hdr) + data_len;

    /* Compute checksum */
    tcp->checksum = tcp_checksum(g_net.ip, conn->remote_ip, pkt, total);

    return ip_send(conn->remote_ip, IP_PROTO_TCP, pkt, total);
}

/* Send a segment AND save it in the retransmit buffer.
   Used for segments that require acknowledgement (SYN, data, FIN). */
static int tcp_send_tracked(struct tcp_conn *conn, uint8_t flags,
                             const void *data, uint16_t data_len,
                             uint32_t expected_ack)
{
    int rc = tcp_send_segment(conn, flags, data, data_len);

    /* Save for retransmission */
    if (data_len <= TCP_RETX_BUF_SIZE) {
        if (data && data_len > 0) {
            const uint8_t *src = (const uint8_t *)data;
            for (uint16_t i = 0; i < data_len; i++)
                conn->retx_buf[i] = src[i];
        }
        conn->retx_len = data_len;
        conn->retx_seq = conn->seq;
        conn->retx_flags = flags;
        conn->retx_count = 0;
        conn->retx_tsc = read_tsc_tcp();
        conn->retx_expected_ack = expected_ack;
    }

    return rc;
}

/* Clear retransmit state (ACK received). */
static void retx_clear(struct tcp_conn *conn)
{
    conn->retx_len = 0;
    conn->retx_count = 0;
    conn->retx_tsc = 0;
    conn->retx_expected_ack = 0;
}

/* ── Initialize a connection slot ────────────── */

static void conn_init(struct tcp_conn *conn, struct ipv4_addr dst,
                       uint16_t port)
{
    conn->remote_ip = dst;
    conn->remote_port = port;
    conn->local_port = 49152 + (uint16_t)(read_tsc_tcp() & 0x3FFF);
    conn->seq = (uint32_t)(read_tsc_tcp() & 0xFFFFFFFF);
    conn->ack = 0;
    conn->state = TCP_CLOSED;
    conn->in_use = 1;
    conn->server_side = 0;
    conn->listener_idx = -1;
    conn->dispatched = 0;
    conn->rx_len = 0;
    conn->rx_read = 0;
    conn->remote_closed = 0;

    /* Clear retransmit state */
    conn->retx_len = 0;
    conn->retx_count = 0;
    conn->retx_tsc = 0;
    conn->retx_expected_ack = 0;
}

/* ── New handle-based API ────────────────────── */

tcp_handle_t tcp_open(struct ipv4_addr dst, uint16_t port)
{
    int slot = conn_alloc();
    if (slot < 0) {
        kputs("tcp: no free connection slots\n");
        return TCP_INVALID_HANDLE;
    }

    struct tcp_conn *conn = &connections[slot];
    conn_init(conn, dst, port);

    /* Send SYN (tracked for retransmission) */
    conn->state = TCP_SYN_SENT;
    uint32_t syn_expected_ack = conn->seq + 1;
    tcp_send_tracked(conn, TCP_SYN, 0, 0, syn_expected_ack);
    conn->seq++;  /* SYN consumes one sequence number */

    /* Wait for SYN-ACK (up to ~5 seconds, retransmits handled by tick) */
    for (int i = 0; i < 50000 && conn->state == TCP_SYN_SENT; i++) {
        net_poll();
        tcp_retransmit_tick();
        for (volatile int j = 0; j < 5000; j++);

        /* Check if retransmit gave up */
        if (conn->state == TCP_ERROR)
            break;
    }

    if (conn->state != TCP_ESTABLISHED) {
        conn->state = TCP_CLOSED;
        conn->in_use = 0;
        return TCP_INVALID_HANDLE;
    }

    return slot;
}

int tcp_send_on(tcp_handle_t h, const void *data, uint16_t len)
{
    struct tcp_conn *conn = conn_get(h);
    if (!conn || conn->state != TCP_ESTABLISHED)
        return -1;

    uint16_t mss = 1460;
    uint16_t sent = 0;

    while (sent < len) {
        uint16_t chunk = len - sent;
        if (chunk > mss) chunk = mss;

        uint32_t expected_ack = conn->seq + chunk;
        tcp_send_tracked(conn, TCP_ACK | TCP_PSH,
                          (const uint8_t *)data + sent, chunk,
                          expected_ack);
        conn->seq += chunk;
        sent += chunk;

        /* Wait for ACK with retransmission support */
        for (int i = 0; i < 50000; i++) {
            net_poll();
            tcp_retransmit_tick();

            /* ACK received — retx cleared by tcp_process */
            if (conn->retx_len == 0)
                break;

            /* Retransmit gave up */
            if (conn->state == TCP_ERROR)
                return -1;

            for (volatile int j = 0; j < 1000; j++);
        }

        if (conn->retx_len != 0) {
            /* Timed out waiting for ACK even after retransmits */
            conn->state = TCP_ERROR;
            return -1;
        }
    }

    return sent;
}

int tcp_recv_on_nb(tcp_handle_t h, void *buf, uint16_t max_len)
{
    struct tcp_conn *conn = conn_get(h);
    if (!conn) return -1;

    /* Return any data already in the buffer */
    if (conn->rx_read < conn->rx_len) {
        uint16_t avail = conn->rx_len - conn->rx_read;
        uint16_t to_copy = avail > max_len ? max_len : avail;
        uint8_t *dst = (uint8_t *)buf;
        for (uint16_t i = 0; i < to_copy; i++)
            dst[i] = conn->rx_buf[conn->rx_read + i];
        conn->rx_read += to_copy;
        return to_copy;
    }
    /* Reset window so future packets land at offset 0. */
    conn->rx_len = 0;
    conn->rx_read = 0;
    return 0;
}

int tcp_send_on_raw(tcp_handle_t h, const void *data, uint16_t len)
{
    struct tcp_conn *conn = conn_get(h);
    if (!conn || conn->state != TCP_ESTABLISHED) return -1;

    uint16_t mss = 1460;
    uint16_t sent = 0;
    while (sent < len) {
        uint16_t chunk = len - sent;
        if (chunk > mss) chunk = mss;
        tcp_send_segment(conn, TCP_ACK | TCP_PSH,
                          (const uint8_t *)data + sent, chunk);
        conn->seq += chunk;
        sent += chunk;
    }
    return sent;
}

int tcp_recv_on(tcp_handle_t h, void *buf, uint16_t max_len)
{
    struct tcp_conn *conn = conn_get(h);
    if (!conn)
        return -1;

    /* Return data already in the buffer */
    if (conn->rx_read < conn->rx_len) {
        uint16_t avail = conn->rx_len - conn->rx_read;
        uint16_t to_copy = avail > max_len ? max_len : avail;
        uint8_t *dst = (uint8_t *)buf;
        for (uint16_t i = 0; i < to_copy; i++)
            dst[i] = conn->rx_buf[conn->rx_read + i];
        conn->rx_read += to_copy;
        return to_copy;
    }

    /* Buffer empty — poll for more data */
    conn->rx_len = 0;
    conn->rx_read = 0;

    /* Poll for incoming data (up to ~5 seconds) */
    for (int i = 0; i < 50000; i++) {
        net_poll();
        tcp_retransmit_tick();

        if (conn->rx_len > 0) {
            uint16_t to_copy = conn->rx_len > max_len ? max_len : conn->rx_len;
            uint8_t *dst = (uint8_t *)buf;
            for (uint16_t j = 0; j < to_copy; j++)
                dst[j] = conn->rx_buf[j];
            conn->rx_read = to_copy;
            return to_copy;
        }

        if (conn->remote_closed || conn->state == TCP_CLOSED ||
            conn->state == TCP_ERROR)
            return 0;

        for (volatile int j = 0; j < 1000; j++);
    }

    return 0;  /* Timeout */
}

void tcp_close_on_async(tcp_handle_t h)
{
    struct tcp_conn *conn = conn_get(h);
    if (!conn) return;
    if (conn->state != TCP_ESTABLISHED && conn->state != TCP_SYN_RECEIVED)
        return;

    conn->state = TCP_FIN_WAIT_1;
    uint32_t fin_expected_ack = conn->seq + 1;
    tcp_send_tracked(conn, TCP_FIN | TCP_ACK, 0, 0, fin_expected_ack);
    conn->seq++;
}

void tcp_close_on(tcp_handle_t h)
{
    struct tcp_conn *conn = conn_get(h);
    if (!conn)
        return;

    if (conn->state == TCP_ESTABLISHED) {
        /* Send FIN (tracked) */
        conn->state = TCP_FIN_WAIT_1;
        uint32_t fin_expected_ack = conn->seq + 1;
        tcp_send_tracked(conn, TCP_FIN | TCP_ACK, 0, 0, fin_expected_ack);
        conn->seq++;

        /* Wait for ACK of FIN */
        for (int i = 0; i < 20000 && conn->state != TCP_CLOSED; i++) {
            net_poll();
            tcp_retransmit_tick();

            if (conn->state == TCP_ERROR)
                break;

            for (volatile int j = 0; j < 1000; j++);
        }
    }

    conn->state = TCP_CLOSED;
    conn->in_use = 0;
    retx_clear(conn);
}

int tcp_is_connected(tcp_handle_t h)
{
    struct tcp_conn *conn = conn_get(h);
    if (!conn)
        return 0;
    return conn->state == TCP_ESTABLISHED;
}

/* ── Retransmission timer tick ───────────────── */

void tcp_retransmit_tick(void)
{
    uint64_t now = read_tsc_tcp();
    uint64_t timeout = retx_timeout_tsc();

    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        struct tcp_conn *conn = &connections[i];
        if (!conn->in_use || conn->retx_len == 0)
            continue;
        if (conn->state == TCP_CLOSED || conn->state == TCP_ERROR)
            continue;

        /* Check if timeout elapsed since last send */
        if ((now - conn->retx_tsc) < timeout)
            continue;

        conn->retx_count++;
        if (conn->retx_count > TCP_RETX_MAX) {
            kputs("tcp: retransmit timeout on port ");
            kput_dec(conn->local_port);
            kputs(" (gave up after ");
            kput_dec(TCP_RETX_MAX);
            kputs(" attempts)\n");
            conn->state = TCP_ERROR;
            retx_clear(conn);
            continue;
        }

        kputs("tcp: retransmit #");
        kput_dec(conn->retx_count);
        kputs(" on port ");
        kput_dec(conn->local_port);
        kputs("\n");

        /* Resend: restore seq to the retx_seq, send, then put seq back.
           We need to emit the exact same segment that was originally sent. */
        uint32_t saved_seq = conn->seq;
        conn->seq = conn->retx_seq;
        tcp_send_segment(conn, conn->retx_flags,
                          conn->retx_len > 0 ? conn->retx_buf : 0,
                          conn->retx_len);
        conn->seq = saved_seq;
        conn->retx_tsc = now;
    }
}

/* ── Legacy wrapper API (slot 0) ─────────────── */

int tcp_connect(struct tcp_conn *conn, struct ipv4_addr dst, uint16_t port)
{
    struct tcp_conn *slot0 = &connections[0];

    /* Initialize slot 0 directly */
    conn_init(slot0, dst, port);

    /* Send SYN (tracked) */
    slot0->state = TCP_SYN_SENT;
    uint32_t syn_expected_ack = slot0->seq + 1;
    tcp_send_tracked(slot0, TCP_SYN, 0, 0, syn_expected_ack);
    slot0->seq++;

    /* Wait for SYN-ACK */
    for (int i = 0; i < 50000 && slot0->state == TCP_SYN_SENT; i++) {
        net_poll();
        tcp_retransmit_tick();
        for (volatile int j = 0; j < 5000; j++);

        if (slot0->state == TCP_ERROR)
            break;
    }

    if (slot0->state != TCP_ESTABLISHED) {
        slot0->state = TCP_CLOSED;
        slot0->in_use = 0;
        /* Mirror failure into caller's struct */
        conn->state = TCP_CLOSED;
        return -1;
    }

    /* Copy internal state back to caller's struct for read access.
       Actual data lives in slot0 — the caller struct is a view. */
    conn->remote_ip = slot0->remote_ip;
    conn->remote_port = slot0->remote_port;
    conn->local_port = slot0->local_port;
    conn->seq = slot0->seq;
    conn->ack = slot0->ack;
    conn->state = slot0->state;
    conn->rx_len = 0;
    conn->rx_read = 0;
    conn->remote_closed = 0;

    return 0;
}

int tcp_send(struct tcp_conn *conn, const void *data, uint16_t len)
{
    (void)conn;  /* All work happens on slot 0 */
    return tcp_send_on(0, data, len);
}

int tcp_recv(struct tcp_conn *conn, void *buf, uint16_t max_len)
{
    (void)conn;
    int rc = tcp_recv_on(0, buf, max_len);

    /* Sync state back so callers checking conn->remote_closed still work */
    struct tcp_conn *slot0 = &connections[0];
    conn->remote_closed = slot0->remote_closed;
    conn->state = slot0->state;

    return rc;
}

/* Non-blocking variant: do a single net_poll to drain pending packets,
 * then return whatever is in the rx buffer. No 5-second wait. mbedtls's
 * BIO callback drives the retry loop via MBEDTLS_ERR_SSL_WANT_READ. */
int tcp_recv_nb(struct tcp_conn *conn, void *buf, uint16_t max_len)
{
    (void)conn;
    struct tcp_conn *slot0 = &connections[0];
    if (!slot0) return -1;

    /* One poll pass to absorb any packet that just arrived. */
    extern void net_poll(void);
    extern void tcp_retransmit_tick(void);
    net_poll();
    tcp_retransmit_tick();

    /* Return data already in the buffer */
    if (slot0->rx_read < slot0->rx_len) {
        uint16_t avail = slot0->rx_len - slot0->rx_read;
        uint16_t to_copy = avail > max_len ? max_len : avail;
        uint8_t *dst = (uint8_t *)buf;
        for (uint16_t i = 0; i < to_copy; i++)
            dst[i] = slot0->rx_buf[slot0->rx_read + i];
        slot0->rx_read += to_copy;
        conn->remote_closed = slot0->remote_closed;
        conn->state = slot0->state;
        return to_copy;
    }

    /* Buffer empty — reset window so future packets land at offset 0. */
    slot0->rx_len = 0;
    slot0->rx_read = 0;
    conn->remote_closed = slot0->remote_closed;
    conn->state = slot0->state;
    return 0;  /* caller polls and retries */
}

int tcp_close(struct tcp_conn *conn)
{
    (void)conn;
    tcp_close_on(0);
    conn->state = TCP_CLOSED;
    return 0;
}

/* ── Server-side listen / accept ─────────────── */

int tcp_listen(uint16_t port, tcp_accept_cb_t accept_cb)
{
    /* Already listening? Update callback and return. */
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (listeners[i].in_use && listeners[i].port == port) {
            listeners[i].accept_cb = accept_cb;
            return 0;
        }
    }
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (!listeners[i].in_use) {
            listeners[i].in_use = 1;
            listeners[i].port = port;
            listeners[i].accept_cb = accept_cb;
            for (int j = 0; j < TCP_LISTEN_BACKLOG; j++)
                listeners[i].pending[j] = -1;
            listeners[i].pending_count = 0;
            listeners[i].accepted_total = 0;
            return 0;
        }
    }
    return -1;
}

int tcp_unlisten(uint16_t port)
{
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (listeners[i].in_use && listeners[i].port == port) {
            listeners[i].in_use = 0;
            listeners[i].port = 0;
            listeners[i].accept_cb = 0;
            for (int j = 0; j < TCP_LISTEN_BACKLOG; j++)
                listeners[i].pending[j] = -1;
            listeners[i].pending_count = 0;
            return 0;
        }
    }
    return -1;
}

int tcp_accept_nb(uint16_t port, tcp_handle_t *out_handle)
{
    if (!out_handle) return -1;
    struct tcp_listener *L = listener_find(port);
    if (!L) return -1;

    for (int i = 0; i < TCP_LISTEN_BACKLOG; i++) {
        int slot = L->pending[i];
        if (slot < 0) continue;
        struct tcp_conn *c = &connections[slot];
        if (!c->in_use) {
            L->pending[i] = -1;
            if (L->pending_count > 0) L->pending_count--;
            continue;
        }
        if (c->state != TCP_ESTABLISHED) continue;

        L->pending[i] = -1;
        if (L->pending_count > 0) L->pending_count--;
        *out_handle = slot;
        return 1;
    }
    return 0;
}

int tcp_listener_count(void)
{
    int n = 0;
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) if (listeners[i].in_use) n++;
    return n;
}

int tcp_accepted_count(void)
{
    int n = 0;
    for (int i = 0; i < TCP_MAX_LISTENERS; i++)
        if (listeners[i].in_use) n += listeners[i].accepted_total;
    return n;
}

/* Allocate a server-side connection slot and arm SYN_RECEIVED state.
 * Returns slot index or -1. */
static int conn_alloc_server(struct ipv4_addr src_ip, uint16_t src_port,
                              uint16_t dst_port, uint32_t client_isn,
                              int listener_idx)
{
    int slot = conn_alloc();
    if (slot < 0) return -1;

    struct tcp_conn *c = &connections[slot];
    c->remote_ip      = src_ip;
    c->remote_port    = src_port;
    c->local_port     = dst_port;
    c->seq            = (uint32_t)(read_tsc_tcp() & 0xFFFFFFFF);
    c->ack            = client_isn + 1;
    c->state          = TCP_SYN_RECEIVED;
    c->in_use         = 1;
    c->server_side    = 1;
    c->listener_idx   = listener_idx;
    c->dispatched     = 0;
    c->rx_len         = 0;
    c->rx_read        = 0;
    c->remote_closed  = 0;
    c->retx_len       = 0;
    c->retx_count     = 0;
    c->retx_tsc       = 0;
    c->retx_expected_ack = 0;
    return slot;
}

/* ── Incoming packet processing ──────────────── */

void tcp_process(const void *frame, uint16_t len)
{
    (void)len;

    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)
        ((const uint8_t *)frame + sizeof(struct eth_hdr));
    uint16_t ip_hdr_len = (ip->ver_ihl & 0x0F) * 4;
    const struct tcp_hdr *tcp = (const struct tcp_hdr *)
        ((const uint8_t *)ip + ip_hdr_len);

    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dst_port);
    uint8_t  flags    = tcp->flags;
    uint32_t seg_seq  = ntohl(tcp->seq);

    /* Demux to the right connection */
    struct tcp_conn *conn = conn_find(ip->src, src_port, dst_port);

    /* No matching conn + SYN-only segment + listener on dst_port =>
     * accept the half-open and reply SYN+ACK. */
    if (!conn && (flags & TCP_SYN) && !(flags & TCP_ACK)) {
        int lidx = listener_idx_for_port(dst_port);
        if (lidx < 0) return;  /* no listener — silently drop (RST optional) */

        int slot = conn_alloc_server(ip->src, src_port, dst_port,
                                      seg_seq, lidx);
        if (slot < 0) return;

        struct tcp_conn *nc = &connections[slot];
        /* Send SYN+ACK (tracked).  seq is our ISN; ack is client ISN+1. */
        uint32_t synack_expected_ack = nc->seq + 1;
        tcp_send_tracked(nc, TCP_SYN | TCP_ACK, 0, 0, synack_expected_ack);
        nc->seq++;  /* SYN consumes one sequence number */
        return;
    }

    if (!conn)
        return;

    uint32_t seg_ack = ntohl(tcp->ack);
    uint16_t tcp_hdr_len = ((tcp->data_off >> 4) & 0x0F) * 4;
    uint16_t ip_total = ntohs(ip->total_len);
    uint16_t data_len = ip_total - ip_hdr_len - tcp_hdr_len;
    const uint8_t *data = (const uint8_t *)tcp + tcp_hdr_len;

    /* RST — close immediately */
    if (flags & TCP_RST) {
        conn->state = TCP_CLOSED;
        conn->remote_closed = 1;
        retx_clear(conn);
        return;
    }

    /* Check if this ACK clears our retransmit buffer */
    if ((flags & TCP_ACK) && conn->retx_len > 0) {
        /* For SYN, the expected_ack is seq+1. For data, seq+data_len.
           Accept if seg_ack >= our expected_ack. */
        if (seg_ack >= conn->retx_expected_ack ||
            /* Handle SYN-ACK special case: retx_len is 0 for SYN but
               retx_expected_ack is set */
            (conn->state == TCP_SYN_SENT && (flags & TCP_SYN)))
            retx_clear(conn);
    }
    /* SYN_RECEIVED: SYN+ACK was tracked with retx_len=0 (no payload). */
    if (conn->state == TCP_SYN_RECEIVED && conn->retx_expected_ack != 0 &&
        (flags & TCP_ACK)) {
        if (seg_ack >= conn->retx_expected_ack)
            retx_clear(conn);
    }
    /* SYN has retx_len=0 (no payload) but retx_expected_ack set.
       Clear on SYN-ACK even though retx_len test above skipped it. */
    if (conn->state == TCP_SYN_SENT && conn->retx_expected_ack != 0 &&
        (flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
        if (seg_ack >= conn->retx_expected_ack)
            retx_clear(conn);
    }

    switch (conn->state) {
    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            conn->ack = seg_seq + 1;
            conn->state = TCP_ESTABLISHED;
            retx_clear(conn);
            /* Send ACK (no tracking needed — pure ACK) */
            tcp_send_segment(conn, TCP_ACK, 0, 0);
        }
        break;

    case TCP_SYN_RECEIVED:
        /* Final ACK of the 3-way handshake completes the connection.
         * Some clients piggyback request data on the same ACK; handle
         * that by falling through to the ESTABLISHED data path below. */
        if ((flags & TCP_ACK) && seg_ack >= conn->retx_expected_ack) {
            retx_clear(conn);
            conn->state = TCP_ESTABLISHED;

            int slot_idx = (int)(conn - connections);
            int lidx = conn->listener_idx;
            if (lidx >= 0 && lidx < TCP_MAX_LISTENERS &&
                listeners[lidx].in_use) {
                listeners[lidx].accepted_total++;
                listener_push(&listeners[lidx], slot_idx);

                /* MasQ: accept = state mutation on the rx pipeline.
                 * Bump CHAIN_NET_RX's vault_version so observers see the
                 * port-level transition. */
                if (CHAIN_NET_RX >= 0) {
                    chain_t *cc = chain_get(CHAIN_NET_RX);
                    if (cc) cc->vault_version++;
                }

                /* Fire accept callback (if set). The callback receives
                 * the new handle and may install its own data handling
                 * (most callers route through the std.http drainer). */
                tcp_accept_cb_t cb = listeners[lidx].accept_cb;
                if (cb) cb((tcp_handle_t)slot_idx);
            }

            /* Same-segment piggybacked data — in-order only (see ESTABLISHED note) */
            if (data_len > 0) {
                if (seg_seq == conn->ack) {
                    uint16_t space = TCP_RX_BUF_SIZE - conn->rx_len;
                    uint16_t to_copy = data_len > space ? space : data_len;
                    for (uint16_t i = 0; i < to_copy; i++)
                        conn->rx_buf[conn->rx_len + i] = data[i];
                    conn->rx_len += to_copy;
                    conn->ack = seg_seq + to_copy;
                }
                tcp_send_segment(conn, TCP_ACK, 0, 0);
            }

            /* FIN piggybacked? */
            if (flags & TCP_FIN) {
                conn->ack = seg_seq + data_len + 1;
                conn->remote_closed = 1;
                tcp_send_segment(conn, TCP_ACK, 0, 0);
                conn->state = TCP_CLOSE_WAIT;
                tcp_send_segment(conn, TCP_FIN | TCP_ACK, 0, 0);
                conn->seq++;
                conn->state = TCP_LAST_ACK;
            }
        }
        break;

    case TCP_ESTABLISHED:
        /* Accept IN-ORDER data only (RFC 793 receive-sequence check). A duplicate
         * or out-of-order segment (seg_seq != rcv_nxt) must NOT be buffered — doing
         * so silently corrupted the stream (double-append on the peer's retransmit,
         * or data landing at the wrong offset). Re-ACK our current rcv_nxt either
         * way so the peer resends exactly the byte we expect. */
        if (data_len > 0) {
            if (seg_seq == conn->ack) {
                uint16_t space = TCP_RX_BUF_SIZE - conn->rx_len;
                uint16_t to_copy = data_len > space ? space : data_len;
                for (uint16_t i = 0; i < to_copy; i++)
                    conn->rx_buf[conn->rx_len + i] = data[i];
                conn->rx_len += to_copy;
                conn->ack = seg_seq + to_copy;   /* ACK only bytes actually buffered */
            }
            tcp_send_segment(conn, TCP_ACK, 0, 0);
        }
        /* FIN from remote */
        if (flags & TCP_FIN) {
            conn->ack = seg_seq + data_len + 1;
            conn->remote_closed = 1;
            tcp_send_segment(conn, TCP_ACK, 0, 0);
            conn->state = TCP_CLOSE_WAIT;
            /* Also close our side */
            tcp_send_segment(conn, TCP_FIN | TCP_ACK, 0, 0);
            conn->seq++;
            conn->state = TCP_LAST_ACK;
        }
        break;

    case TCP_FIN_WAIT_1:
        if (flags & TCP_ACK) {
            retx_clear(conn);
            if (flags & TCP_FIN) {
                conn->ack = seg_seq + 1;
                tcp_send_segment(conn, TCP_ACK, 0, 0);
                conn->state = TCP_CLOSED;
                conn->in_use = 0;
            } else {
                conn->state = TCP_FIN_WAIT_2;
            }
        }
        break;

    case TCP_FIN_WAIT_2:
        if (flags & TCP_FIN) {
            conn->ack = seg_seq + 1;
            tcp_send_segment(conn, TCP_ACK, 0, 0);
            conn->state = TCP_CLOSED;
            conn->in_use = 0;
        }
        break;

    case TCP_LAST_ACK:
        if (flags & TCP_ACK) {
            conn->state = TCP_CLOSED;
            conn->in_use = 0;
            retx_clear(conn);
        }
        break;

    default:
        break;
    }
}
