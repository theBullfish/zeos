/*
 * Zeos — TLS Layer (mbedTLS integration)
 *
 * This file provides the glue between mbedTLS and the Zeos
 * bare-metal environment. mbedTLS does all the crypto —
 * we just provide:
 *   1. Network I/O callbacks (tcp_send/tcp_recv)
 *   2. Memory allocation (heap.c)
 *   3. Entropy (TSC + MAC)
 *   4. Certificate trust store
 *
 * Build: mbedTLS source goes in kernel/lib/mbedtls/
 *        Configure via kernel/lib/mbedtls/mbedtls_config.h
 *
 * Until mbedTLS is integrated, this file compiles but
 * the functions return errors. The API is stable — once
 * mbedTLS drops in, these implementations fill out.
 */

#include "net_tls.h"
#include "net_tcp.h"
#include "net_dns.h"
#include "kprint.h"

/* ── mbedTLS headers (when integrated) ──
 *
 * #include "mbedtls/ssl.h"
 * #include "mbedtls/entropy.h"
 * #include "mbedtls/ctr_drbg.h"
 * #include "mbedtls/x509_crt.h"
 * #include "mbedtls/net_sockets.h"
 */

/* ── TLS connection structure ── */

struct tls_conn {
    /* mbedTLS state (populated when library is integrated)
     *
     * mbedtls_ssl_context      ssl;
     * mbedtls_ssl_config       conf;
     */
    int tcp_connected;
    char hostname[256];
    uint16_t port;
    int active;
};

/* Single connection (matches TCP limitation) */
static struct tls_conn g_tls;

/* ── Platform callbacks for mbedTLS ── */

/*
 * mbedTLS calls these for network I/O. We route to our TCP stack.
 *
 * int zeos_tls_send(void *ctx, const unsigned char *buf, size_t len) {
 *     (void)ctx;
 *     return tcp_send(buf, (uint16_t)len);
 * }
 *
 * int zeos_tls_recv(void *ctx, unsigned char *buf, size_t len) {
 *     (void)ctx;
 *     return tcp_recv(buf, (uint16_t)len);
 * }
 */

/*
 * Entropy source: TSC provides high-resolution timing jitter.
 * Combined with MAC address for device uniqueness.
 *
 * int zeos_entropy_source(void *data, unsigned char *output,
 *                         size_t len, size_t *olen) {
 *     uint32_t lo, hi;
 *     for (size_t i = 0; i < len; i += 4) {
 *         __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
 *         uint32_t val = lo ^ (hi << 16) ^ g_net.mac.b[i % 6];
 *         size_t remaining = len - i;
 *         size_t chunk = remaining < 4 ? remaining : 4;
 *         for (size_t j = 0; j < chunk; j++)
 *             output[i + j] = (val >> (j * 8)) & 0xFF;
 *     }
 *     *olen = len;
 *     return 0;
 * }
 */

/* ── Public API ── */

int tls_init(void)
{
    kputs("TLS: subsystem ready (mbedTLS integration pending)\n");

    /* When mbedTLS is integrated:
     *
     * mbedtls_ssl_config_init(&g_ssl_conf);
     * mbedtls_x509_crt_init(&g_ca_certs);
     * mbedtls_ctr_drbg_init(&g_ctr_drbg);
     * mbedtls_entropy_init(&g_entropy);
     *
     * // Add our TSC entropy source
     * mbedtls_entropy_add_source(&g_entropy, zeos_entropy_source,
     *                            NULL, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);
     *
     * // Seed the DRBG
     * mbedtls_ctr_drbg_seed(&g_ctr_drbg, mbedtls_entropy_func,
     *                        &g_entropy, "zeos-tls", 8);
     *
     * // Load Mozilla root CAs (embedded in binary)
     * mbedtls_x509_crt_parse(&g_ca_certs, mozilla_ca_bundle,
     *                         mozilla_ca_bundle_len);
     *
     * // Configure TLS 1.3
     * mbedtls_ssl_config_defaults(&g_ssl_conf,
     *     MBEDTLS_SSL_IS_CLIENT,
     *     MBEDTLS_SSL_TRANSPORT_STREAM,
     *     MBEDTLS_SSL_PRESET_DEFAULT);
     * mbedtls_ssl_conf_min_tls_version(&g_ssl_conf,
     *     MBEDTLS_SSL_VERSION_TLS1_3);
     * mbedtls_ssl_conf_authmode(&g_ssl_conf,
     *     MBEDTLS_SSL_VERIFY_REQUIRED);
     * mbedtls_ssl_conf_ca_chain(&g_ssl_conf, &g_ca_certs, NULL);
     * mbedtls_ssl_conf_rng(&g_ssl_conf, mbedtls_ctr_drbg_random,
     *                       &g_ctr_drbg);
     */

    return 0;
}

tls_conn_t *tls_connect(const char *hostname, uint16_t port)
{
    kputs("TLS: connect ");
    kputs(hostname);
    kputs(":");
    kput_dec(port);
    kputs(" (mbedTLS not yet integrated)\n");

    /* When mbedTLS is integrated:
     *
     * // DNS resolve
     * struct ipv4_addr server_ip;
     * if (dns_resolve(hostname, &server_ip) < 0) return NULL;
     *
     * // TCP connect
     * if (tcp_connect(server_ip, port) < 0) return NULL;
     *
     * // Set up SSL context
     * mbedtls_ssl_init(&g_tls.ssl);
     * mbedtls_ssl_setup(&g_tls.ssl, &g_ssl_conf);
     * mbedtls_ssl_set_hostname(&g_tls.ssl, hostname);  // SNI
     *
     * // Set I/O callbacks
     * mbedtls_ssl_set_bio(&g_tls.ssl, NULL,
     *                      zeos_tls_send, zeos_tls_recv, NULL);
     *
     * // TLS handshake
     * int ret;
     * while ((ret = mbedtls_ssl_handshake(&g_tls.ssl)) != 0) {
     *     if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
     *         ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
     *         kputs("TLS: handshake failed\n");
     *         tcp_close();
     *         return NULL;
     *     }
     * }
     *
     * // Verify certificate
     * if (mbedtls_ssl_get_verify_result(&g_tls.ssl) != 0) {
     *     kputs("TLS: certificate verification failed\n");
     *     mbedtls_ssl_close_notify(&g_tls.ssl);
     *     tcp_close();
     *     return NULL;
     * }
     *
     * g_tls.active = 1;
     * return &g_tls;
     */

    return 0;  /* Not yet available */
}

int tls_send(tls_conn_t *conn, const void *data, int len)
{
    if (!conn || !conn->active) return -1;

    /* return mbedtls_ssl_write(&conn->ssl, data, len); */
    (void)data; (void)len;
    return -1;
}

int tls_recv(tls_conn_t *conn, void *buf, int max_len)
{
    if (!conn || !conn->active) return -1;

    /* return mbedtls_ssl_read(&conn->ssl, buf, max_len); */
    (void)buf; (void)max_len;
    return -1;
}

void tls_close(tls_conn_t *conn)
{
    if (!conn || !conn->active) return;

    /* mbedtls_ssl_close_notify(&conn->ssl);
     * mbedtls_ssl_free(&conn->ssl);
     * tcp_close();
     */

    conn->active = 0;
    kputs("TLS: connection closed\n");
}

/* ── HTTPS convenience ── */

static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    *dst = 0;
}

static void str_append(char *dst, const char *src) {
    while (*dst) dst++;
    while (*src) *dst++ = *src++;
    *dst = 0;
}

int https_get(const char *hostname, const char *path,
              char *resp_buf, int resp_max, int *body_len)
{
    tls_conn_t *conn = tls_connect(hostname, 443);
    if (!conn) return -1;

    /* Build HTTP/1.1 request */
    char req[1024];
    req[0] = 0;
    str_copy(req, "GET ");
    str_append(req, path);
    str_append(req, " HTTP/1.1\r\nHost: ");
    str_append(req, hostname);
    str_append(req, "\r\nConnection: close\r\n"
                    "User-Agent: Zeos/0.1\r\n\r\n");

    int req_len = str_len(req);
    if (tls_send(conn, req, req_len) < 0) {
        tls_close(conn);
        return -1;
    }

    /* Read response */
    int total = 0;
    while (total < resp_max - 1) {
        int n = tls_recv(conn, resp_buf + total, resp_max - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    resp_buf[total] = 0;

    tls_close(conn);

    /* Parse status code */
    int status = -1;
    if (total > 12 && resp_buf[0] == 'H') {
        status = (resp_buf[9] - '0') * 100 +
                 (resp_buf[10] - '0') * 10 +
                 (resp_buf[11] - '0');
    }

    /* Find body */
    if (body_len) {
        *body_len = 0;
        for (int i = 0; i < total - 3; i++) {
            if (resp_buf[i] == '\r' && resp_buf[i+1] == '\n' &&
                resp_buf[i+2] == '\r' && resp_buf[i+3] == '\n') {
                *body_len = total - (i + 4);
                /* Shift body to start of buffer */
                char *body = resp_buf + i + 4;
                for (int j = 0; j < *body_len; j++)
                    resp_buf[j] = body[j];
                resp_buf[*body_len] = 0;
                break;
            }
        }
    }

    return status;
}
