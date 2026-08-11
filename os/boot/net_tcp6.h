/*
 * Zeos -- TCP over IPv6 (parallel to net_tcp.c).
 *
 * Mirrors the v4 TCP state machine but with its own slot pool. The v4
 * pool stays untouched. AF_INET / AF_INET6 distinction lives in the
 * caller -- you call tcp_open() for v4 and tcp_open_v6() for v6.
 */

#ifndef ZEOS_NET_TCP6_H
#define ZEOS_NET_TCP6_H

#include "net_ipv6.h"
#include "net_tcp.h"   /* for tcp_handle_t / tcp_state / TCP_INVALID_HANDLE */

/* v6 handles share the int-handle representation but live in a
 * separate slot pool. Aliased for callers that want family clarity
 * (e.g. tls_transport_t's v6 arm). */
typedef tcp_handle_t tcp_handle6_t;

/* Open a TCP/IPv6 connection. Returns handle or TCP_INVALID_HANDLE. */
tcp_handle_t tcp_open_v6(struct ipv6_addr dst, uint16_t port);

/* Open with a budget (milliseconds). Returns TCP_INVALID_HANDLE if the
 * SYN-ACK doesn't arrive before the budget expires. Used by Happy
 * Eyeballs to bound how long we wait on v6 before falling back to v4. */
tcp_handle_t tcp_open_v6_timed(struct ipv6_addr dst, uint16_t port,
                                uint32_t budget_ms);

/* Non-blocking receive on a v6 handle. Single net_poll, returns whatever's
 * already in the rx buffer. 0 = no data yet (caller should retry).
 * < 0 = error or remote closed (peer dropped us). */
int  tcp_recv_v6_nb(tcp_handle_t h, void *buf, uint16_t max_len);

/* Probe remote_closed flag on a v6 handle. */
int  tcp_v6_remote_closed(tcp_handle_t h);

/* Send data on a v6 handle. Blocking stop-and-wait with retransmission. */
int  tcp_send_v6(tcp_handle_t h, const void *data, uint16_t len);

/* Receive data on a v6 handle. */
int  tcp_recv_v6(tcp_handle_t h, void *buf, uint16_t max_len);

/* Close a v6 handle. */
void tcp_close_v6(tcp_handle_t h);

/* Tick retransmission timers (v6 pool). */
void tcp6_retransmit_tick(void);

/* Incoming TCP/IPv6 segment dispatch (called from net_ipv6_process). */
void tcp6_process(const struct ipv6_addr *src, const struct ipv6_addr *dst,
                  const void *tcp_pkt, uint16_t tcp_len);

#endif /* ZEOS_NET_TCP6_H */
