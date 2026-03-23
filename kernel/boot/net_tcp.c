/*
 * Zeos — Minimal TCP implementation
 *
 * Single active connection. Stop-and-wait. Happy path only.
 * Enough to do HTTP GET on port 80.
 */

#include "net_tcp.h"
#include "net_ip.h"
#include "kprint.h"

/* The active connection (only one at a time) */
static struct tcp_conn *active_conn = 0;

static uint64_t read_tsc_tcp(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* TCP pseudo-header for checksum */
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
    tcp->window = htons(4096);
    tcp->checksum = 0;
    tcp->urgent = 0;

    /* Copy data */
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

int tcp_connect(struct tcp_conn *conn, struct ipv4_addr dst, uint16_t port)
{
    /* Initialize connection */
    conn->remote_ip = dst;
    conn->remote_port = port;
    conn->local_port = 49152 + (uint16_t)(read_tsc_tcp() & 0x3FFF);  /* Ephemeral port */
    conn->seq = (uint32_t)(read_tsc_tcp() & 0xFFFFFFFF);
    conn->ack = 0;
    conn->state = TCP_CLOSED;
    conn->rx_len = 0;
    conn->rx_read = 0;
    conn->remote_closed = 0;

    active_conn = conn;

    /* Send SYN */
    conn->state = TCP_SYN_SENT;
    tcp_send_segment(conn, TCP_SYN, 0, 0);
    conn->seq++;  /* SYN consumes one sequence number */

    /* Wait for SYN-ACK */
    for (int i = 0; i < 50000 && conn->state == TCP_SYN_SENT; i++) {
        net_poll();
        for (volatile int j = 0; j < 5000; j++);
    }

    if (conn->state != TCP_ESTABLISHED) {
        conn->state = TCP_CLOSED;
        active_conn = 0;
        return -1;
    }

    return 0;
}

int tcp_send(struct tcp_conn *conn, const void *data, uint16_t len)
{
    if (conn->state != TCP_ESTABLISHED)
        return -1;

    /* Send in segments up to MSS (1460 for 1500 MTU - 20 IP - 20 TCP) */
    uint16_t mss = 1460;
    uint16_t sent = 0;

    while (sent < len) {
        uint16_t chunk = len - sent;
        if (chunk > mss) chunk = mss;

        tcp_send_segment(conn, TCP_ACK | TCP_PSH,
                          (const uint8_t *)data + sent, chunk);
        conn->seq += chunk;
        sent += chunk;

        /* Wait for ACK (simple stop-and-wait) */
        uint32_t expected_ack = conn->seq;
        (void)expected_ack;
        for (int i = 0; i < 20000; i++) {
            net_poll();
            /* The ACK handler in tcp_process updates our state */
            for (volatile int j = 0; j < 1000; j++);
        }
    }

    return sent;
}

int tcp_recv(struct tcp_conn *conn, void *buf, uint16_t max_len)
{
    /* First, return data already in the buffer */
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

    /* Poll for incoming data (up to 5 seconds) */
    for (int i = 0; i < 50000; i++) {
        net_poll();

        if (conn->rx_len > 0) {
            uint16_t to_copy = conn->rx_len > max_len ? max_len : conn->rx_len;
            uint8_t *dst = (uint8_t *)buf;
            for (uint16_t j = 0; j < to_copy; j++)
                dst[j] = conn->rx_buf[j];
            conn->rx_read = to_copy;
            return to_copy;
        }

        if (conn->remote_closed || conn->state == TCP_CLOSED)
            return 0;

        for (volatile int j = 0; j < 1000; j++);
    }

    return 0;  /* Timeout */
}

int tcp_close(struct tcp_conn *conn)
{
    if (conn->state == TCP_ESTABLISHED) {
        /* Send FIN */
        conn->state = TCP_FIN_WAIT_1;
        tcp_send_segment(conn, TCP_FIN | TCP_ACK, 0, 0);
        conn->seq++;

        /* Wait for ACK of FIN */
        for (int i = 0; i < 20000 && conn->state != TCP_CLOSED; i++) {
            net_poll();
            for (volatile int j = 0; j < 1000; j++);
        }
    }

    conn->state = TCP_CLOSED;
    if (active_conn == conn)
        active_conn = 0;
    return 0;
}

void tcp_process(const void *frame, uint16_t len)
{
    (void)len;
    if (!active_conn) return;

    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)((const uint8_t *)frame + sizeof(struct eth_hdr));
    uint16_t ip_hdr_len = (ip->ver_ihl & 0x0F) * 4;
    const struct tcp_hdr *tcp = (const struct tcp_hdr *)((const uint8_t *)ip + ip_hdr_len);

    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dst_port);

    /* Match to our connection */
    struct tcp_conn *conn = active_conn;
    if (dst_port != conn->local_port || src_port != conn->remote_port)
        return;
    if (!ip_eq(ip->src, conn->remote_ip))
        return;

    uint32_t seg_seq = ntohl(tcp->seq);
    (void)ntohl(tcp->ack);  /* seg_ack tracked implicitly */
    uint8_t flags = tcp->flags;
    uint16_t tcp_hdr_len = ((tcp->data_off >> 4) & 0x0F) * 4;
    uint16_t ip_total = ntohs(ip->total_len);
    uint16_t data_len = ip_total - ip_hdr_len - tcp_hdr_len;
    const uint8_t *data = (const uint8_t *)tcp + tcp_hdr_len;

    /* RST — close immediately */
    if (flags & TCP_RST) {
        conn->state = TCP_CLOSED;
        conn->remote_closed = 1;
        return;
    }

    switch (conn->state) {
    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            conn->ack = seg_seq + 1;
            conn->state = TCP_ESTABLISHED;
            /* Send ACK */
            tcp_send_segment(conn, TCP_ACK, 0, 0);
        }
        break;

    case TCP_ESTABLISHED:
        /* ACK incoming data */
        if (data_len > 0) {
            /* Copy to receive buffer */
            uint16_t space = sizeof(conn->rx_buf) - conn->rx_len;
            uint16_t to_copy = data_len > space ? space : data_len;
            for (uint16_t i = 0; i < to_copy; i++)
                conn->rx_buf[conn->rx_len + i] = data[i];
            conn->rx_len += to_copy;
            conn->ack = seg_seq + data_len;
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
            if (flags & TCP_FIN) {
                conn->ack = seg_seq + 1;
                tcp_send_segment(conn, TCP_ACK, 0, 0);
                conn->state = TCP_CLOSED;
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
        }
        break;

    case TCP_LAST_ACK:
        if (flags & TCP_ACK) {
            conn->state = TCP_CLOSED;
        }
        break;

    default:
        break;
    }
}
