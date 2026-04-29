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
 */

#include "net_tls.h"
#include "net_tcp.h"
#include "net_dns.h"
#include "kprint.h"

/* ── mbedTLS headers ── */
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/platform.h"
#include "psa/crypto.h"

/* Platform shims provided by mbedtls_platform.c */
extern void *zeos_calloc(size_t n, size_t size);
extern void  zeos_free(void *ptr);
extern int   snprintf(char *buf, size_t size, const char *fmt, ...);
extern int   vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);
extern long  zeos_time(long *timer);

/* ── Root CA trust anchors (PEM-encoded) ── */
#include "ca_bundle.h"

/* ── TLS connection structure ── */

struct tls_conn {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;
    struct tcp_conn     tcp;
    int tcp_connected;
    char hostname[256];
    uint16_t port;
    int active;
};

/* Single connection (matches TCP limitation) */
static struct tls_conn g_tls;

/* ── Global mbedTLS state ── */
static mbedtls_x509_crt      g_ca_certs;
static mbedtls_ctr_drbg_context g_ctr_drbg;
static mbedtls_entropy_context  g_entropy;
static mbedtls_ssl_config       g_ssl_conf;

/* ── Platform callbacks for mbedTLS ── */

/*
 * mbedTLS calls these for network I/O. We route to our TCP stack.
 * The ctx pointer carries the tcp_conn that owns this TLS session.
 */
static int zeos_tls_send(void *ctx, const unsigned char *buf, size_t len)
{
    struct tcp_conn *tcp = (struct tcp_conn *)ctx;
    return tcp_send(tcp, buf, (uint16_t)len);
}

static int zeos_tls_recv(void *ctx, unsigned char *buf, size_t len)
{
    struct tcp_conn *tcp = (struct tcp_conn *)ctx;
    return tcp_recv(tcp, buf, (uint16_t)len);
}

/* ── Public API ── */

int tls_init(void)
{
    /* Register our platform shims with mbedTLS before anything else. */
    mbedtls_platform_set_calloc_free(zeos_calloc, zeos_free);
    mbedtls_platform_set_snprintf(snprintf);
    mbedtls_platform_set_vsnprintf(vsnprintf);
    mbedtls_platform_set_time((mbedtls_time_t (*)(mbedtls_time_t *))zeos_time);

    /* PSA crypto — required by TLS 1.3 in mbedTLS 3.6 */
    psa_status_t pst = psa_crypto_init();
    if (pst != PSA_SUCCESS) {
        kputs("TLS: PSA init failed (");
        kput_dec(-(int)pst);
        kputs(")\n");
        return -1;
    }

    /* Initialize all mbedTLS contexts */
    mbedtls_ssl_config_init(&g_ssl_conf);
    mbedtls_x509_crt_init(&g_ca_certs);
    mbedtls_ctr_drbg_init(&g_ctr_drbg);
    mbedtls_entropy_init(&g_entropy);

    /* Seed the DRBG (entropy comes from mbedtls_hardware_poll
     * via MBEDTLS_ENTROPY_HARDWARE_ALT in mbedtls_platform.c) */
    int ret = mbedtls_ctr_drbg_seed(&g_ctr_drbg, mbedtls_entropy_func,
                                     &g_entropy,
                                     (const unsigned char *)"zeos-tls", 8);
    if (ret != 0) {
        kputs("TLS: DRBG seed failed (");
        kput_dec(-ret);
        kputs(")\n");
        return -1;
    }

    /* Load root CA trust anchors */
    ret = mbedtls_x509_crt_parse(&g_ca_certs,
                                  (const unsigned char *)ca_bundle_pem,
                                  sizeof(ca_bundle_pem));
    if (ret != 0) {
        kputs("TLS: CA parse failed (");
        kput_dec(-ret);
        kputs(")\n");
        return -1;
    }

    /* Configure as TLS 1.2/1.3 client */
    ret = mbedtls_ssl_config_defaults(&g_ssl_conf,
              MBEDTLS_SSL_IS_CLIENT,
              MBEDTLS_SSL_TRANSPORT_STREAM,
              MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        kputs("TLS: config defaults failed\n");
        return -1;
    }

    mbedtls_ssl_conf_authmode(&g_ssl_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&g_ssl_conf, &g_ca_certs, NULL);
    mbedtls_ssl_conf_rng(&g_ssl_conf, mbedtls_ctr_drbg_random, &g_ctr_drbg);

    kputs("TLS: subsystem ready (mbedTLS ");
    kputs(MBEDTLS_VERSION_STRING);
    kputs(")\n");

    return 0;
}

tls_conn_t *tls_connect(const char *hostname, uint16_t port)
{
    kputs("TLS: connect ");
    kputs(hostname);
    kputs(":");
    kput_dec(port);
    kputs("\n");

    /* DNS resolve */
    struct ipv4_addr server_ip;
    if (dns_resolve(hostname, &server_ip) < 0) {
        kputs("TLS: DNS failed\n");
        return NULL;
    }

    /* TCP connect */
    if (tcp_connect(&g_tls.tcp, server_ip, port) < 0) {
        kputs("TLS: TCP connect failed\n");
        return NULL;
    }
    g_tls.tcp_connected = 1;

    /* Copy hostname for SNI */
    int i;
    for (i = 0; hostname[i] && i < 255; i++)
        g_tls.hostname[i] = hostname[i];
    g_tls.hostname[i] = 0;
    g_tls.port = port;

    /* Set up SSL context */
    mbedtls_ssl_init(&g_tls.ssl);

    int ret = mbedtls_ssl_setup(&g_tls.ssl, &g_ssl_conf);
    if (ret != 0) {
        kputs("TLS: ssl_setup failed\n");
        tcp_close(&g_tls.tcp);
        return NULL;
    }

    /* SNI — server name indication */
    ret = mbedtls_ssl_set_hostname(&g_tls.ssl, g_tls.hostname);
    if (ret != 0) {
        kputs("TLS: set_hostname failed\n");
        mbedtls_ssl_free(&g_tls.ssl);
        tcp_close(&g_tls.tcp);
        return NULL;
    }

    /* Set I/O callbacks — ctx is the tcp_conn */
    mbedtls_ssl_set_bio(&g_tls.ssl, &g_tls.tcp,
                         zeos_tls_send, zeos_tls_recv, NULL);

    /* TLS handshake */
    while ((ret = mbedtls_ssl_handshake(&g_tls.ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            kputs("TLS: handshake failed (");
            kput_dec(-ret);
            kputs(")\n");
            mbedtls_ssl_free(&g_tls.ssl);
            tcp_close(&g_tls.tcp);
            return NULL;
        }
    }

    /* Verify certificate */
    uint32_t flags = mbedtls_ssl_get_verify_result(&g_tls.ssl);
    if (flags != 0) {
        kputs("TLS: certificate verification failed (0x");
        kput_hex(flags);
        kputs(")\n");
        mbedtls_ssl_close_notify(&g_tls.ssl);
        mbedtls_ssl_free(&g_tls.ssl);
        tcp_close(&g_tls.tcp);
        return NULL;
    }

    kputs("TLS: connected, ");
    kputs(mbedtls_ssl_get_version(&g_tls.ssl));
    kputs("\n");

    g_tls.active = 1;
    return &g_tls;
}

int tls_send(tls_conn_t *conn, const void *data, int len)
{
    if (!conn || !conn->active) return -1;

    return mbedtls_ssl_write(&conn->ssl, (const unsigned char *)data, len);
}

int tls_recv(tls_conn_t *conn, void *buf, int max_len)
{
    if (!conn || !conn->active) return -1;

    int ret = mbedtls_ssl_read(&conn->ssl, (unsigned char *)buf, max_len);

    /* Translate mbedTLS EOF to our convention (0 = EOF) */
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
        ret == MBEDTLS_ERR_SSL_CONN_EOF)
        return 0;

    return ret;
}

void tls_close(tls_conn_t *conn)
{
    if (!conn || !conn->active) return;

    /* Send close_notify to peer */
    mbedtls_ssl_close_notify(&conn->ssl);
    mbedtls_ssl_free(&conn->ssl);
    tcp_close(&conn->tcp);

    conn->active = 0;
    conn->tcp_connected = 0;
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
