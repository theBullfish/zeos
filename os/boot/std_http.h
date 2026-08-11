/*
 * Zeos — std.http (server side)
 *
 * Listener registry + request parser + accept-loop drain.
 *
 * zp_http_listen(port, chain_id) registers a listener record and also
 * arms a real TCP listen on the same port (tcp_listen).  When a SYN
 * arrives on that port the kernel TCP state machine completes the
 * three-way handshake and pushes the new connection to the listener's
 * pending queue; the accept callback marks the handle as HTTP-owned.
 * zp_http_poll() drains owned handles each net_poll tick: it parses
 * the request line, runs the registered chain (or sends a default
 * 200/ok response when chain_id < 0), and closes the connection.
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

/* Drain accepted server connections: parse HTTP/1.1 request lines off
 * each owned connection, fire the registered chain, send the response.
 * Called from net_poll(). */
void zp_http_poll(void);

/* Selftest counters. */
int  zp_http_accepted_total(void);
int  zp_http_responses_total(void);

#endif
