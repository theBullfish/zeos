/*
 * Zeos — TLS Layer
 *
 * Wraps mbedTLS for HTTPS support. Provides tls_connect/send/recv/close
 * that layer on top of the existing TCP stack.
 *
 * mbedTLS was chosen because:
 *   - Designed for bare-metal / embedded (no OS required)
 *   - ~100KB footprint (configurable)
 *   - TLS 1.3 support
 *   - Maintained by ARM, battle-tested
 *   - Apache 2.0 license
 *
 * Integration:
 *   - Memory: uses heap.c (zeos_malloc/zeos_free)
 *   - Entropy: TSC + virtio-net MAC as seed
 *   - Network I/O: callbacks into tcp_send/tcp_recv
 *   - Certificates: embedded root CA bundle (Mozilla's)
 *   - Time: TSC-derived (approximate, sufficient for cert validation)
 */

#ifndef ZEOS_NET_TLS_H
#define ZEOS_NET_TLS_H

#include "net.h"
#include <stdint.h>

/* TLS connection state */
typedef struct tls_conn tls_conn_t;

/*
 * Establish a TLS connection to host:port.
 * Performs DNS resolution, TCP connect, TLS handshake,
 * and certificate validation.
 *
 * Returns a tls_conn handle or NULL on failure.
 * hostname is used for SNI (Server Name Indication).
 */
tls_conn_t *tls_connect(const char *hostname, uint16_t port);

/*
 * Send data over TLS. Returns bytes sent or -1 on error.
 */
int tls_send(tls_conn_t *conn, const void *data, int len);

/*
 * Receive data over TLS. Returns bytes received, 0 on EOF, -1 on error.
 * Blocks until data available or timeout.
 */
int tls_recv(tls_conn_t *conn, void *buf, int max_len);

/*
 * Close TLS connection (sends close_notify, then TCP FIN).
 */
void tls_close(tls_conn_t *conn);

/*
 * Initialize the TLS subsystem.
 * Seeds entropy, loads certificate store.
 * Called once during net_init().
 */
int tls_init(void);

/* ── HTTPS convenience ── */

/*
 * HTTPS GET — like http_get() but over TLS.
 * hostname: e.g. "example.com"
 * path: e.g. "/index.html"
 * resp_buf: output buffer for response body
 * resp_max: size of resp_buf
 * Returns HTTP status code (200, 404, etc.) or -1 on connection failure.
 */
int https_get(const char *hostname, const char *path,
              char *resp_buf, int resp_max, int *body_len);

/*
 * Single-hop HTTPS GET. body_len receives the body length.
 * On a 3xx response, redir_loc receives the raw Location value
 * (NUL-terminated, may be relative). Used internally to drive
 * cross-scheme redirect chains.
 */
int https_get_once(const char *hostname, const char *path,
                   char *resp_buf, int resp_max, int *body_len,
                   char *redir_loc, int redir_max);

#endif /* ZEOS_NET_TLS_H */
