/*
 * Zeos — std.http (server side)
 *
 * Listener registry + request parser.  Today the kernel TCP layer
 * (net_tcp.c) only exposes outbound connections (tcp_open / tcp_send_on).
 * std.http registers listener records and parses inbound HTTP/1.1
 * requests off any TCP handle that the runtime hands it (via
 * zp_http_feed_handle).  The chain wiring layer is the "accept loop":
 * when a new tcp handle is established, it is fed to the matching
 * listener and a typed http_request is emitted on the registered chain.
 *
 * Honest gap: until net_tcp grows a real listen+accept primitive, no
 * unsolicited inbound SYN can arrive, so the listener will not see real
 * traffic in QEMU.  All registered listeners and the routing table are
 * inspectable; the parser and respond path are real.
 */

#ifndef ZEOS_STD_HTTP_H
#define ZEOS_STD_HTTP_H

#include <stdint.h>

#define HTTP_MAX_LISTENERS  8
#define HTTP_MAX_ROUTES     32
#define HTTP_REQ_BUF        2048

struct http_listener {
    int      in_use;
    uint16_t port;
    int      chain_id;
};

struct http_route {
    int     in_use;
    char    method[8];
    char    path  [64];
    int     chain_id;
};

/* Idempotent. */
void zp_http_init(void);

int  zp_http_listen(uint16_t port, int chain_id);
int  zp_http_route(const char *method, const char *path, int chain_id);

/* Write an HTTP response on a tcp handle. Returns bytes written or -1. */
int  zp_http_respond(int tcp_handle, int status, const char *body, int body_len,
                     const char *content_type);

/* Counts (selftest / introspection). */
int  zp_http_listener_count(void);
int  zp_http_route_count(void);

/* Parse a raw HTTP/1.1 request buffer into method+path. Returns 0 on
 * success, -1 on malformed input. */
int  zp_http_parse_request(const char *buf, int buf_len,
                           char *method, int method_max,
                           char *path,   int path_max);

#endif
