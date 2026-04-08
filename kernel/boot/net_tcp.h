/*
 * Zeos — TCP (multi-connection with retransmission)
 *
 * Supports up to TCP_MAX_CONNECTIONS concurrent connections.
 * Each connection has its own state machine, seq/ack, and rx buffer.
 * Basic retransmission: unacked segments retransmit up to 3 times at ~1s intervals.
 *
 * New handle-based API (tcp_open/tcp_send_on/tcp_recv_on/tcp_close_on).
 * Old pointer-based API (tcp_connect/tcp_send/tcp_recv/tcp_close) preserved
 * as wrappers using connection slot 0.
 */

#ifndef ZEOS_NET_TCP_H
#define ZEOS_NET_TCP_H

#include "net.h"

/* ── Limits ──────────────────────────────────── */

#define TCP_MAX_CONNECTIONS  8
#define TCP_RX_BUF_SIZE      16384
#define TCP_RETX_BUF_SIZE    1500    /* Max segment to retransmit */
#define TCP_RETX_MAX         3       /* Retransmit attempts before error */

/* ── TCP header ──────────────────────────────── */

struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;    /* Upper 4 bits = offset in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

/* TCP flags */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

/* ── Connection states ───────────────────────── */

enum tcp_state {
    TCP_CLOSED,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_ERROR,          /* Retransmission timeout or fatal error */
};

/* ── Connection handle ───────────────────────── */

typedef int tcp_handle_t;

#define TCP_INVALID_HANDLE  (-1)

/* ── Connection structure ────────────────────── */

struct tcp_conn {
    struct ipv4_addr remote_ip;
    uint16_t         local_port;
    uint16_t         remote_port;
    uint32_t         seq;           /* Our next sequence number */
    uint32_t         ack;           /* Next expected from remote */
    enum tcp_state   state;
    int              in_use;        /* Slot occupied? */

    /* Receive buffer */
    uint8_t          rx_buf[TCP_RX_BUF_SIZE];
    uint16_t         rx_len;        /* Bytes in receive buffer */
    uint16_t         rx_read;       /* Read cursor */
    int              remote_closed; /* Remote sent FIN */

    /* Retransmit buffer (last unacked outgoing segment) */
    uint8_t          retx_buf[TCP_RETX_BUF_SIZE];
    uint16_t         retx_len;      /* 0 = nothing pending */
    uint32_t         retx_seq;      /* Seq number of pending segment */
    uint8_t          retx_flags;    /* Flags of pending segment */
    uint8_t          retx_count;    /* Attempts so far */
    uint64_t         retx_tsc;      /* TSC when last sent */
    uint32_t         retx_expected_ack; /* ACK we need to clear retx */
};

/* ── New handle-based API ────────────────────── */

/* Open a connection to dst:port. Returns handle or TCP_INVALID_HANDLE. Blocking. */
tcp_handle_t tcp_open(struct ipv4_addr dst, uint16_t port);

/* Send data on a handle. Blocking (stop-and-wait with retransmission). */
int tcp_send_on(tcp_handle_t h, const void *data, uint16_t len);

/* Receive data on a handle. Returns bytes received, 0 on close/timeout. */
int tcp_recv_on(tcp_handle_t h, void *buf, uint16_t max_len);

/* Close a connection by handle. */
void tcp_close_on(tcp_handle_t h);

/* Check if a handle is connected. */
int tcp_is_connected(tcp_handle_t h);

/* Tick retransmission timers. Call from net_poll or periodically. */
void tcp_retransmit_tick(void);

/* ── Legacy API (backwards compatible) ───────── */

/*
 * These wrap the handle-based API using slot 0 as the default.
 * Existing code (HTTP, TLS) can keep using struct tcp_conn pointers
 * — tcp_connect() fills in the caller's struct AND occupies slot 0.
 */
int tcp_connect(struct tcp_conn *conn, struct ipv4_addr dst, uint16_t port);
int tcp_send(struct tcp_conn *conn, const void *data, uint16_t len);
int tcp_recv(struct tcp_conn *conn, void *buf, uint16_t max_len);
int tcp_close(struct tcp_conn *conn);

/* ── Incoming packet handler ─────────────────── */

/* Process incoming TCP segment (called by ip_process). */
void tcp_process(const void *frame, uint16_t len);

#endif /* ZEOS_NET_TCP_H */
