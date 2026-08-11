/*
 * Zeos — std.tls server-side termination.
 *
 * tls.listen(port, cert_path, key_path, chain_id) registers a server
 * listener on `port` that:
 *   - tcp_listen on port
 *   - on accept: run mbedtls server-side handshake (MBEDTLS_SSL_IS_SERVER)
 *     using cert+key loaded from the FAT32-backed vault paths
 *   - on handshake success: dispatch to handler chain (chain_id)
 *
 * Cert + key are loaded once per port at listen time, parsed via
 * mbedtls_x509_crt_parse and mbedtls_pk_parse_key, and the resulting
 * pk_context is wrapped in a CFA SOVEREIGN handle (mirroring the
 * tls_init() pattern in net_tls.c). The cert is INTERNAL — it's public
 * material; the key is SOVEREIGN — only sovereign observers ever see it.
 *
 * Honest gaps:
 *   - cert generation (a `--gen-cert` follow-up in the shell) is not in
 *     this surface. Operators bring their own cert+key today.
 *   - one cert/key per port. SNI multi-cert is a follow-up.
 */

#ifndef ZEOS_STD_TLS_SERVER_H
#define ZEOS_STD_TLS_SERVER_H

#include <stdint.h>

#define TLS_SRV_MAX_LISTENERS  4
#define TLS_SRV_MAX_SESSIONS   4
#define TLS_SRV_REQ_BUF        2048

/* Idempotent. Initializes listener/session pools. */
void tls_srv_init(void);

/*
 * Arm a TLS server listener.
 *   port      — TCP port to listen on (typically 443)
 *   cert_path — vault path to PEM-encoded server cert
 *   key_path  — vault path to PEM-encoded private key
 *   chain_id  — handler chain to dispatch on each accepted handshake
 *
 * Returns listener slot index on success, -1 on failure (cert/key
 * load/parse error, port-already-listening, table full).
 */
int  tls_srv_listen(uint16_t port, const char *cert_path,
                    const char *key_path, int chain_id);

/* Drain accepted handshakes and dispatch. Called from net_poll(). */
void tls_srv_poll(void);

/* Selftest counters. */
int  tls_srv_listener_count(void);
int  tls_srv_handshakes_total(void);
int  tls_srv_responses_total(void);

#endif /* ZEOS_STD_TLS_SERVER_H */
