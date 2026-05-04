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
#include "net_http.h"
#include "kprint.h"
#include "cfa_handle.h"
#include "chain.h"
#include "chain_registry.h"

/* ── mbedTLS headers ── */
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/platform.h"
#include "mbedtls/net_sockets.h"
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
    /* Per-session CFA handle wrapping &ssl. Populated at tls_init for
     * every slot in the pool so the handle table reports session
     * coverage even when no connection is live. cfa_resolve through
     * this is the only path internal tls_send/recv/close use. */
    cfa_handle_t h_ssl;
};

/* TLS connection pool. The transport layer is still single-active
 * (TCP can host one stream at a time), but having multiple slots lets
 * the CFA handle table reflect that the kernel is willing to terminate
 * N independent TLS sessions and each one has its own MasQ-tier-gated
 * ssl_context. Bump if more concurrent sessions are ever needed. */
#define TLS_POOL_SIZE 4
static struct tls_conn g_tls_pool[TLS_POOL_SIZE];
/* Backwards-compat alias: pre-existing code paths reach for g_tls. */
#define g_tls (g_tls_pool[0])

/* ── Global mbedTLS state ── */
static mbedtls_x509_crt      g_ca_certs;
static mbedtls_ctr_drbg_context g_ctr_drbg;
static mbedtls_entropy_context  g_entropy;
static mbedtls_ssl_config       g_ssl_conf;

/*
 * CFA handles for the security-relevant TLS state. The raw structs
 * still live in BSS (mbedTLS APIs require flat pointers), but every
 * access in this file goes through cfa_resolve() so MasQ-tier rules
 * gate observers that lack INTERNAL/SOVEREIGN perception. CFA address
 * is derived from CHAIN_NET_TX -- TLS is the upper layer of network.
 */
static cfa_handle_t g_h_conf;     /* wraps g_ssl_conf */

/* Resolve helpers. Before tls_init() finishes wrapping the statics,
 * the handles are 0 and we fall through to the raw struct so the
 * mbedtls init calls work. After wrapping, every access goes through
 * cfa_resolve(): a wrong-tier observer gets NULL and the path returns
 * an error rather than silently leaking the keys. */
static mbedtls_ssl_config *tls_conf(void)
{
    if (!g_h_conf) return &g_ssl_conf;  /* pre-wrap: init path */
    return (mbedtls_ssl_config *)cfa_resolve(g_h_conf);
}

static mbedtls_ssl_context *tls_ssl(struct tls_conn *conn)
{
    if (!conn) return (mbedtls_ssl_context *)0;
    if (!conn->h_ssl) return &conn->ssl;  /* pre-wrap: setup path */
    return (mbedtls_ssl_context *)cfa_resolve(conn->h_ssl);
}

/* ── Platform callbacks for mbedTLS ── */

/*
 * mbedTLS calls these for network I/O. We route to our TCP stack and
 * translate Zeos return semantics into mbedtls's expected sentinel
 * values so the handshake state machine can differentiate "no data
 * yet, retry" from "peer closed" from "fatal error".
 */
static int zeos_tls_send(void *ctx, const unsigned char *buf, size_t len)
{
    struct tcp_conn *tcp = (struct tcp_conn *)ctx;
    /* Cap to uint16_t; mbedtls will call again for the rest. */
    uint16_t chunk = len > 0xFFFF ? 0xFFFF : (uint16_t)len;
    int n = tcp_send(tcp, buf, chunk);
    if (n > 0)              return n;
    if (tcp->remote_closed) return MBEDTLS_ERR_NET_CONN_RESET;
    if (n == 0)             return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int zeos_tls_recv(void *ctx, unsigned char *buf, size_t len)
{
    struct tcp_conn *tcp = (struct tcp_conn *)ctx;
    uint16_t chunk = len > 0xFFFF ? 0xFFFF : (uint16_t)len;
    /* Non-blocking: single net_poll pass, return whatever's available.
     * mbedtls's WANT_READ retry loop drives the wait — no 5-sec inline poll. */
    int n = tcp_recv_nb(tcp, buf, chunk);
    if (n > 0)              return n;
    if (tcp->remote_closed) return 0;  /* clean EOF */
    if (n == 0)             return MBEDTLS_ERR_SSL_WANT_READ;  /* no data yet, retry */
    return MBEDTLS_ERR_NET_RECV_FAILED;
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

    /* Load root CA trust anchors. mbedtls_x509_crt_parse returns 0 on
     * full success, a positive count if SOME certs in the PEM bundle
     * failed to parse but others succeeded (still useful), or a negative
     * error if nothing parsed. Only the last is fatal. */
    ret = mbedtls_x509_crt_parse(&g_ca_certs,
                                  (const unsigned char *)ca_bundle_pem,
                                  sizeof(ca_bundle_pem));
    if (ret < 0) {
        kputs("TLS: CA parse failed (");
        kput_dec((unsigned long)-ret);
        kputs(")\n");
        return -1;
    }
    if (ret > 0) {
        kputs("TLS: ");
        kput_dec((unsigned long)ret);
        kputs(" cert(s) in bundle failed to parse — others ok\n");
        /* Sanity: refuse to come up if half the bundle failed — almost
         * certainly a corrupted bundle, not just the usual SHA-1 misses. */
        if (ret > 50) {
            kputs("TLS: bundle health check FAILED — too many parse errors\n");
            return -1;
        }
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

    /* Wrap the SSL config in a CFA handle. CFA address is derived from
     * CHAIN_NET_TX (TLS is the upper layer of network). If the net
     * chain hasn't registered yet (early init), use a synthetic root
     * address so the handle still exists; rebinding from net_tx is a
     * later optimization. Tier INTERNAL: kernel TLS state is private
     * to internal/sovereign observers. */
    {
        cfa_addr_t conf_addr;
        chain_t *net_tx = (CHAIN_NET_TX >= 0) ? chain_get(CHAIN_NET_TX) : (chain_t *)0;
        if (net_tx) {
            conf_addr = cfa_derive(&net_tx->addr, 1);  /* child slot 1 = ssl_config */
        } else {
            int j;
            for (j = 0; j < 8; j++) conf_addr.segments[j] = 0;
            conf_addr.segments[0] = 0xFF;  /* synthetic TLS root */
            conf_addr.segments[1] = 1;
            conf_addr.depth = 2;
            conf_addr.birth_tsc = 0;
        }
        g_h_conf = cfa_wrap_cat(&g_ssl_conf, sizeof(g_ssl_conf),
                                conf_addr, MASQ_INTERNAL, CFA_CAT_TLS_CONF);
        if (!g_h_conf) {
            kputs("TLS: cfa_wrap(ssl_conf) failed\n");
            return -1;
        }
    }

    /* Pre-wrap each session slot's ssl_context. The struct is zeroed
     * BSS at this point; mbedtls_ssl_init will be called against it
     * inside tls_connect. The handle stays live for the lifetime of
     * the kernel so the MasQ tier check applies whether or not the
     * slot is currently carrying a live handshake. */
    for (int s = 0; s < TLS_POOL_SIZE; s++) {
        struct tls_conn *slot = &g_tls_pool[s];
        cfa_addr_t ssl_addr;
        chain_t *net_tx = (CHAIN_NET_TX >= 0) ? chain_get(CHAIN_NET_TX) : (chain_t *)0;
        if (net_tx) {
            /* child slot 0x100 + s = ssl session */
            ssl_addr = cfa_derive(&net_tx->addr, (uint32_t)(0x100 + s));
        } else {
            int j;
            for (j = 0; j < 8; j++) ssl_addr.segments[j] = 0;
            ssl_addr.segments[0] = 0xFF;
            ssl_addr.segments[1] = (uint32_t)(0x100 + s);
            ssl_addr.depth = 2;
            ssl_addr.birth_tsc = 0;
        }
        slot->h_ssl = cfa_wrap_cat(&slot->ssl, sizeof(slot->ssl),
                                   ssl_addr, MASQ_INTERNAL,
                                   CFA_CAT_TLS_SESSION);
        if (!slot->h_ssl) {
            kputs("TLS: cfa_wrap(session) failed\n");
            return -1;
        }
    }

    kputs("TLS: subsystem ready (mbedTLS ");
    kputs(MBEDTLS_VERSION_STRING);
    kputs("), epoch=");
    kput_dec((unsigned long)zeos_time(0));
    kputs(", cfa_handle=conf\n");

    return 0;
}

tls_conn_t *tls_connect(const char *hostname, uint16_t port)
{
    /* Single-connection model: refuse a second connect while one is live
     * rather than silently overwriting the global ssl context. */
    if (g_tls.active) {
        kputs("TLS: refused — connection already active\n");
        return NULL;
    }

    /* Reject overlong hostnames before SNI truncation could mask cert
     * mismatches. SNI buffer is 256 bytes; cert SAN matching also needs
     * the full name to be meaningful. */
    size_t hlen = 0;
    while (hostname[hlen] && hlen < 512) hlen++;
    if (hlen == 0 || hlen >= 256) {
        kputs("TLS: hostname length out of range\n");
        return NULL;
    }

    kputs("TLS: connect ");
    kputs(hostname);
    kputs(":");
    kput_dec(port);
    kputs("\n");

    /* If hostname is an IPv4 dotted-quad, skip DNS. Cap each octet's
     * digit count at 3 so the unsigned accumulator can't wrap. */
    struct ipv4_addr server_ip;
    {
        unsigned a, b, c, d;
        const char *p = hostname;
        int parsed = 0;
        a = 0; while (*p >= '0' && *p <= '9' && parsed < 4) { a = a*10 + (unsigned)(*p - '0'); p++; parsed++; }
        if (parsed && parsed < 4 && *p == '.' && a < 256) {
            p++; parsed = 0; b = 0;
            while (*p >= '0' && *p <= '9' && parsed < 4) { b = b*10 + (unsigned)(*p - '0'); p++; parsed++; }
            if (parsed && parsed < 4 && *p == '.' && b < 256) {
                p++; parsed = 0; c = 0;
                while (*p >= '0' && *p <= '9' && parsed < 4) { c = c*10 + (unsigned)(*p - '0'); p++; parsed++; }
                if (parsed && parsed < 4 && *p == '.' && c < 256) {
                    p++; parsed = 0; d = 0;
                    while (*p >= '0' && *p <= '9' && parsed < 4) { d = d*10 + (unsigned)(*p - '0'); p++; parsed++; }
                    if (parsed && parsed < 4 && *p == 0 && d < 256) {
                        server_ip.b[0] = (uint8_t)a;
                        server_ip.b[1] = (uint8_t)b;
                        server_ip.b[2] = (uint8_t)c;
                        server_ip.b[3] = (uint8_t)d;
                        goto have_ip;
                    }
                }
            }
        }
    }
    if (dns_resolve(hostname, &server_ip) < 0) {
        kputs("TLS: DNS failed\n");
        return NULL;
    }
have_ip:;

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

    /* Set up SSL context. The slot's CFA handle was wrapped at
     * tls_init time; we just initialize the underlying struct here.
     * Every subsequent access goes through cfa_resolve via tls_ssl(). */
    mbedtls_ssl_init(&g_tls.ssl);

    mbedtls_ssl_context *ssl = tls_ssl(&g_tls);
    mbedtls_ssl_config  *conf = tls_conf();
    if (!ssl || !conf) {
        kputs("TLS: cfa_resolve denied during connect\n");
        mbedtls_ssl_free(&g_tls.ssl);
        tcp_close(&g_tls.tcp);
        return NULL;
    }

    int ret = mbedtls_ssl_setup(ssl, conf);
    if (ret != 0) {
        kputs("TLS: ssl_setup failed\n");
        mbedtls_ssl_free(ssl);
        tcp_close(&g_tls.tcp);
        return NULL;
    }

    /* SNI — server name indication */
    ret = mbedtls_ssl_set_hostname(ssl, g_tls.hostname);
    if (ret != 0) {
        kputs("TLS: set_hostname failed\n");
        mbedtls_ssl_free(ssl);
        tcp_close(&g_tls.tcp);
        return NULL;
    }

    /* Set I/O callbacks — ctx is the tcp_conn */
    mbedtls_ssl_set_bio(ssl, &g_tls.tcp,
                         zeos_tls_send, zeos_tls_recv, NULL);

    /* TLS handshake */
    while ((ret = mbedtls_ssl_handshake(ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            kputs("TLS: handshake failed (");
            kput_dec(-ret);
            kputs(")");
            if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
                uint32_t f = mbedtls_ssl_get_verify_result(ssl);
                kputs(" verify_flags=0x");
                kput_hex(f);
            }
            kputs("\n");
            mbedtls_ssl_free(ssl);
            tcp_close(&g_tls.tcp);
            return NULL;
        }
    }

    /* Verify certificate */
    uint32_t flags = mbedtls_ssl_get_verify_result(ssl);
    if (flags != 0) {
        kputs("TLS: certificate verification failed (0x");
        kput_hex(flags);
        kputs(")\n");
        mbedtls_ssl_close_notify(ssl);
        mbedtls_ssl_free(ssl);
        tcp_close(&g_tls.tcp);
        return NULL;
    }

    kputs("TLS: connected, ");
    kputs(mbedtls_ssl_get_version(ssl));
    kputs("\n");

    g_tls.active = 1;
    return &g_tls;
}

int tls_send(tls_conn_t *conn, const void *data, int len)
{
    if (!conn || !conn->active) return -1;
    mbedtls_ssl_context *ssl = tls_ssl(conn);
    if (!ssl) return -1;  /* MasQ denied */

    return mbedtls_ssl_write(ssl, (const unsigned char *)data, len);
}

int tls_recv(tls_conn_t *conn, void *buf, int max_len)
{
    if (!conn || !conn->active) return -1;
    mbedtls_ssl_context *ssl = tls_ssl(conn);
    if (!ssl) return -1;  /* MasQ denied */

    int ret = mbedtls_ssl_read(ssl, (unsigned char *)buf, max_len);

    /* Translate mbedTLS EOF to our convention (0 = EOF) */
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
        ret == MBEDTLS_ERR_SSL_CONN_EOF)
        return 0;

    return ret;
}

void tls_close(tls_conn_t *conn)
{
    if (!conn || !conn->active) return;
    mbedtls_ssl_context *ssl = tls_ssl(conn);

    /* Send close_notify to peer */
    if (ssl) {
        mbedtls_ssl_close_notify(ssl);
        mbedtls_ssl_free(ssl);
    }
    tcp_close(&conn->tcp);

    /* The slot's h_ssl is permanent for the slot lifetime. We don't
     * release it here -- the next tls_connect on this slot will
     * mbedtls_ssl_init() the same struct and the existing handle keeps
     * pointing at the same memory. */

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

#define HTTPS_MAX_REDIRECTS 5

static int str_starts_ci_t(const char *s, const char *prefix) {
    while (*prefix) {
        char a = *s++, b = *prefix++;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static int find_substr_ci(const char *buf, int len, const char *name,
                          const char **out_end)
{
    int nlen = 0; while (name[nlen]) nlen++;
    for (int i = 0; i < len - nlen; i++) {
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            char a = buf[i + j], b = name[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = 0; break; }
        }
        if (match) {
            if (out_end) *out_end = buf + i + nlen;
            return i;
        }
    }
    return -1;
}

static void tls_resolve_location(int base_use_tls, const char *base_host,
                                 const char *base_path, const char *loc,
                                 char *out_url, int max)
{
    int o = 0;
    if (str_starts_ci_t(loc, "http://") || str_starts_ci_t(loc, "https://")) {
        while (*loc && o < max - 1) out_url[o++] = *loc++;
        out_url[o] = 0;
        return;
    }
    const char *scheme = base_use_tls ? "https://" : "http://";
    while (*scheme && o < max - 1) out_url[o++] = *scheme++;

    if (loc[0] == '/' && loc[1] == '/') {
        loc += 2;
        while (*loc && o < max - 1) out_url[o++] = *loc++;
        out_url[o] = 0;
        return;
    }

    while (*base_host && o < max - 1) out_url[o++] = *base_host++;
    if (loc[0] == '/') {
        while (*loc && o < max - 1) out_url[o++] = *loc++;
    } else {
        int last_slash = 0;
        for (int i = 0; base_path[i]; i++) if (base_path[i] == '/') last_slash = i;
        for (int i = 0; i <= last_slash && o < max - 1; i++) out_url[o++] = base_path[i];
        while (*loc && o < max - 1) out_url[o++] = *loc++;
    }
    out_url[o] = 0;
}

static void tls_split_url(const char *url, int *use_tls,
                          char *host, int hmax, char *path, int pmax)
{
    *use_tls = 0;
    const char *p = url;
    if (str_starts_ci_t(p, "https://")) { *use_tls = 1; p += 8; }
    else if (str_starts_ci_t(p, "http://")) { p += 7; }

    int hi = 0;
    while (*p && *p != '/' && *p != ':' && hi < hmax - 1) host[hi++] = *p++;
    host[hi] = 0;
    if (*p == ':') { while (*p && *p != '/') p++; }
    if (*p == '/') {
        int pi = 0;
        while (*p && pi < pmax - 1) path[pi++] = *p++;
        path[pi] = 0;
    } else {
        path[0] = '/'; path[1] = 0;
    }
}

/*
 * Single-hop HTTPS GET. body_len receives the body length.
 * On a 3xx response, redir_loc receives the raw Location value
 * (NUL-terminated, may be relative). Returns status or -1.
 */
int https_get_once(const char *hostname, const char *path,
                   char *resp_buf, int resp_max, int *body_len,
                   char *redir_loc, int redir_max)
{
    if (redir_max > 0) redir_loc[0] = 0;
    if (body_len) *body_len = 0;

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

    /* Send: retry on WANT_READ/WRITE, accept partial writes. */
    int req_len = str_len(req);
    int sent = 0;
    while (sent < req_len) {
        int n = tls_send(conn, req + sent, req_len - sent);
        if (n == MBEDTLS_ERR_SSL_WANT_READ ||
            n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (n < 0) {
            tls_close(conn);
            return -1;
        }
        sent += n;
    }

    /* Read response */
    int total = 0;
    while (total < resp_max - 1) {
        int n = tls_recv(conn, resp_buf + total, resp_max - 1 - total);
        if (n == MBEDTLS_ERR_SSL_WANT_READ ||
            n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;  /* retry */
        if (n <= 0) break;  /* 0 = clean EOF, < 0 = fatal */
        total += n;
    }
    resp_buf[total] = 0;

    tls_close(conn);

    /* Parse status code: require "HTTP/x.y NNN" minimum (12 chars + space)
     * and validate that bytes 9-11 are decimal digits before parsing. */
    int status = -1;
    if (total >= 12 && resp_buf[0] == 'H' &&
        resp_buf[9]  >= '0' && resp_buf[9]  <= '9' &&
        resp_buf[10] >= '0' && resp_buf[10] <= '9' &&
        resp_buf[11] >= '0' && resp_buf[11] <= '9') {
        status = (resp_buf[9] - '0') * 100 +
                 (resp_buf[10] - '0') * 10 +
                 (resp_buf[11] - '0');
    }

    /* Extract Location header from raw response (before we shift the body) */
    if (redir_max > 0) {
        const char *after = 0;
        int idx = find_substr_ci(resp_buf, total, "\nlocation: ", &after);
        if (idx >= 0 && after) {
            const char *p = after;
            while (*p == ' ' || *p == '\t') p++;
            int li = 0;
            while (p < resp_buf + total && *p != '\r' && *p != '\n' &&
                   li < redir_max - 1) {
                redir_loc[li++] = *p++;
            }
            redir_loc[li] = 0;
        }
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

/* Forward decl from net_http.c */
extern int http_get_once_loc(const char *host, const char *path,
                             struct http_response *resp,
                             char *redir_loc, int redir_max);

int https_get(const char *hostname, const char *path,
              char *resp_buf, int resp_max, int *body_len)
{
    static char cur_host[256];
    static char cur_path[1024];
    int  cur_tls = 1;

    int i = 0;
    while (hostname[i] && i < 255) { cur_host[i] = hostname[i]; i++; }
    cur_host[i] = 0;
    i = 0;
    while (path[i] && i < 1023) { cur_path[i] = path[i]; i++; }
    cur_path[i] = 0;

    for (int hop = 0; hop <= HTTPS_MAX_REDIRECTS; hop++) {
        static char redir[2048]; redir[0] = 0;

        if (cur_tls) {
            int status = https_get_once(cur_host, cur_path, resp_buf, resp_max,
                                         body_len, redir, sizeof(redir));
            if (status < 0) return -1;

            if ((status == 301 || status == 302 || status == 307 || status == 308) &&
                redir[0] && hop < HTTPS_MAX_REDIRECTS) {
                static char absu[2048];
                tls_resolve_location(1, cur_host, cur_path, redir, absu, sizeof(absu));
                kputs("HTTPS: redirect -> "); kputs(absu); kputs("\n");
                tls_split_url(absu, &cur_tls, cur_host, 256, cur_path, 1024);
                continue;
            }
            return status;
        }

        /* Cross-scheme: HTTPS → HTTP. Dispatch via http_get_once_loc and copy.
         * http_response is HTTP_MAX_BODY (256 KB) — must not live on the stack. */
        static struct http_response hresp;
        int rc = http_get_once_loc(cur_host, cur_path, &hresp, redir, sizeof(redir));
        if (rc < 0) return -1;
        int sc = hresp.status_code;

        if ((sc == 301 || sc == 302 || sc == 307 || sc == 308) &&
            redir[0] && hop < HTTPS_MAX_REDIRECTS) {
            static char absu[2048];
            tls_resolve_location(0, cur_host, cur_path, redir, absu, sizeof(absu));
            kputs("HTTPS: redirect -> "); kputs(absu); kputs("\n");
            tls_split_url(absu, &cur_tls, cur_host, 256, cur_path, 1024);
            continue;
        }

        int copy = (int)hresp.body_len;
        if (copy > resp_max - 1) copy = resp_max - 1;
        for (int k = 0; k < copy; k++) resp_buf[k] = hresp.body[k];
        resp_buf[copy] = 0;
        if (body_len) *body_len = copy;
        return sc;
    }

    kputs("HTTPS: redirect limit reached\n");
    return -1;
}
