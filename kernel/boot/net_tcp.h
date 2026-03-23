/*
 * Zeos — Minimal TCP
 *
 * Single-connection, synchronous, stop-and-wait.
 * Enough for HTTP GET. Not a full TCP stack.
 */

#ifndef ZEOS_NET_TCP_H
#define ZEOS_NET_TCP_H

#include "net.h"

/* TCP header */
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

/* TCP connection states */
enum tcp_state {
    TCP_CLOSED,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
};

/* TCP connection */
struct tcp_conn {
    struct ipv4_addr remote_ip;
    uint16_t         local_port;
    uint16_t         remote_port;
    uint32_t         seq;           /* Our next sequence number */
    uint32_t         ack;           /* Next expected from remote */
    enum tcp_state   state;
    uint8_t          rx_buf[16384]; /* Receive buffer */
    uint16_t         rx_len;        /* Bytes in receive buffer */
    uint16_t         rx_read;       /* Read cursor */
    int              remote_closed; /* Remote sent FIN */
};

/* Connect to a remote host. Blocking (SYN → SYN-ACK). */
int tcp_connect(struct tcp_conn *conn, struct ipv4_addr dst, uint16_t port);

/* Send data. Blocking (waits for ACK). */
int tcp_send(struct tcp_conn *conn, const void *data, uint16_t len);

/* Receive data. Returns bytes received, or 0 if no data / connection closed. */
int tcp_recv(struct tcp_conn *conn, void *buf, uint16_t max_len);

/* Close the connection (FIN exchange). */
int tcp_close(struct tcp_conn *conn);

/* Process incoming TCP segment (called by ip_process). */
void tcp_process(const void *frame, uint16_t len);

#endif /* ZEOS_NET_TCP_H */
