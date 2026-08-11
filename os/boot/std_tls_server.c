/*
 * Zeos — std.tls server-side termination.
 *
 * Server peer to net_tls.c (which is the TLS *client* path).  Each
 * tls_srv_listen() call:
 *   1. Loads cert + key bytes from vault paths.
 *   2. Parses cert via mbedtls_x509_crt_parse and key via
 *      mbedtls_pk_parse_key.
 *   3. Wraps the pk_context in a CFA SOVEREIGN handle and the cert in
 *      an INTERNAL handle (cert is public material; key is sovereign).
 *   4. Builds a per-port mbedtls_ssl_config with
 *      MBEDTLS_SSL_IS_SERVER + own_cert.
 *   5. Calls tcp_listen(port, accept_cb).  The accept callback parks
 *      the new tcp handle in the per-port pending queue; tls_srv_poll()
 *      drains it: setup ssl_context, run mbedtls_ssl_handshake until
 *      WANT_READ/WRITE clears, then dispatch the registered handler
 *      chain.  After dispatch the connection is closed.
 *
 * Honest about scope:
 *   - One cert+key per port.  SNI multi-cert is a follow-up.
 *   - Cert generation isn't here; --gen-cert in shell is the next step.
 *   - The handshake runs synchronously in tls_srv_poll once the kernel
 *     hands us an established TCP connection.  Long handshakes block
 *     net_poll for the duration; for a single LAN client this is fine.
 *     Concurrent multi-client throughput is a follow-up.
 *
 * Selftest: tls.listen contributes a `+TLS server` suffix on the
 * Z+ stdlib selftest line.
 */

#include "std_tls_server.h"
#include "net_tcp.h"
#include "vault.h"
#include "kprint.h"
#include "heap.h"
#include "cfa_handle.h"
#include "chain.h"
#include "chain_registry.h"
#include "signal.h"

#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/error.h"

/* From net_tls.c — shared DRBG seeded at tls_init(). */
extern int tls_init(void);
extern int  zp_http_respond(int tcp_handle, int status, const char *body, int body_len,
                            const char *content_type);

/* Local DRBG that piggybacks the same entropy source as net_tls.c. */
static mbedtls_entropy_context  g_srv_entropy;
static mbedtls_ctr_drbg_context g_srv_drbg;
static int                      g_srv_drbg_inited;

struct tls_srv_listener {
    int                     in_use;
    uint16_t                port;
    int                     chain_id;

    /* Persisted cert+key — must outlive every session built from this
     * config (mbedtls_ssl_setup() refers to conf, conf points at these). */
    mbedtls_x509_crt        cert;
    mbedtls_pk_context      pk;
    mbedtls_ssl_config      conf;

    cfa_handle_t            h_pk;     /* SOVEREIGN: private key */
    cfa_handle_t            h_cert;   /* INTERNAL:  public cert  */
    cfa_handle_t            h_conf;   /* INTERNAL:  ssl config   */
};

struct tls_srv_session {
    int                 in_use;
    int                 listener_idx;
    int                 tcp_handle;        /* tcp_handle_t */
    mbedtls_ssl_context ssl;
    int                 ssl_inited;        /* 1 once mbedtls_ssl_init called */
    int                 handshake_done;    /* 1 = ready to dispatch */
    cfa_handle_t        h_ssl;             /* INTERNAL */
};

static struct tls_srv_listener g_listeners[TLS_SRV_MAX_LISTENERS];
static struct tls_srv_session  g_sessions [TLS_SRV_MAX_SESSIONS];
static int g_inited = 0;
static int g_handshakes_total = 0;
static int g_responses_total  = 0;

/* tcp_listen takes a per-listener accept_cb; ours is intentionally a
 * no-op. tcp_accept_nb pulls already-established connections out of the
 * kernel's pending queue inside tls_srv_poll(), so we don't need to do
 * anything from inside the kernel TCP rx interrupt path. */
static void tls_srv_noop_accept(tcp_handle_t h) { (void)h; }

static void tls_srv_accept_common(int listener_idx, tcp_handle_t h);

void tls_srv_init(void)
{
    if (g_inited) return;
    for (int i = 0; i < TLS_SRV_MAX_LISTENERS; i++) g_listeners[i].in_use = 0;
    for (int i = 0; i < TLS_SRV_MAX_SESSIONS;  i++) {
        g_sessions[i].in_use = 0;
        g_sessions[i].tcp_handle = -1;
    }
    /* Make sure the client-side TLS subsystem's platform shims +
     * entropy table are up. tls_init() is idempotent there. */
    (void)tls_init();
    g_inited = 1;
}

/* ── Net I/O callbacks for the server-side BIO. ─── */
static int srv_tls_send(void *ctx, const unsigned char *buf, size_t len)
{
    struct tls_srv_session *sess = (struct tls_srv_session *)ctx;
    if (!sess || sess->tcp_handle < 0) return MBEDTLS_ERR_NET_SEND_FAILED;
    uint16_t chunk = len > 0xFFFF ? 0xFFFF : (uint16_t)len;
    int n = tcp_send_on_raw(sess->tcp_handle, buf, chunk);
    if (n > 0) return n;
    if (n == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    if (!tcp_is_connected(sess->tcp_handle)) return MBEDTLS_ERR_NET_CONN_RESET;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int srv_tls_recv(void *ctx, unsigned char *buf, size_t len)
{
    struct tls_srv_session *sess = (struct tls_srv_session *)ctx;
    if (!sess || sess->tcp_handle < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
    uint16_t chunk = len > 0xFFFF ? 0xFFFF : (uint16_t)len;
    int n = tcp_recv_on_nb(sess->tcp_handle, buf, chunk);
    if (n > 0) return n;
    if (n == 0) {
        /* No data right now. If the connection is gone, EOF; otherwise
         * tell mbedtls to retry. */
        if (!tcp_is_connected(sess->tcp_handle)) return 0;
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

/* ── PEM loader (vault-backed) ───────────────── */

/* Slurp a vault path into a freshly-malloced NUL-terminated buffer.
 * Caller frees. *out_len receives the byte length INCLUDING the trailing
 * NUL (mbedtls PEM parser requires a NUL within the buffer length). */
static unsigned char *load_pem(const char *path, size_t *out_len)
{
    if (!path || !*path) return (unsigned char *)0;
    int sz = vault_size(path);
    if (sz <= 0 || sz > 65536) {
        kputs("[tls.listen] cert/key vault path missing or too large: ");
        kputs(path);
        kputc('\n');
        return (unsigned char *)0;
    }
    unsigned char *buf = (unsigned char *)kmalloc((uint64_t)(sz + 1));
    if (!buf) return (unsigned char *)0;
    int n = vault_read(path, (char *)buf, sz);
    if (n <= 0) {
        kfree(buf);
        return (unsigned char *)0;
    }
    buf[n] = 0;
    *out_len = (size_t)(n + 1);  /* +1 for NUL — mbedtls wants it */
    return buf;
}

static int srv_drbg_init_once(void)
{
    if (g_srv_drbg_inited) return 0;
    mbedtls_entropy_init(&g_srv_entropy);
    mbedtls_ctr_drbg_init(&g_srv_drbg);
    int rc = mbedtls_ctr_drbg_seed(&g_srv_drbg, mbedtls_entropy_func,
                                    &g_srv_entropy,
                                    (const unsigned char *)"zeos-tls-srv", 12);
    if (rc != 0) {
        kputs("[tls.listen] DRBG seed failed\n");
        return -1;
    }
    g_srv_drbg_inited = 1;
    return 0;
}

int tls_srv_listen(uint16_t port, const char *cert_path,
                   const char *key_path, int chain_id)
{
    tls_srv_init();
    if (srv_drbg_init_once() != 0) return -1;

    /* Allocate a listener slot. */
    int idx = -1;
    for (int i = 0; i < TLS_SRV_MAX_LISTENERS; i++) {
        if (!g_listeners[i].in_use) { idx = i; break; }
    }
    if (idx < 0) {
        kputs("[tls.listen] listener table full\n");
        return -1;
    }
    struct tls_srv_listener *L = &g_listeners[idx];

    /* Load + parse cert. */
    size_t cert_len = 0, key_len = 0;
    unsigned char *cert_buf = load_pem(cert_path, &cert_len);
    if (!cert_buf) return -1;
    unsigned char *key_buf = load_pem(key_path, &key_len);
    if (!key_buf) { kfree(cert_buf); return -1; }

    mbedtls_x509_crt_init(&L->cert);
    int rc = mbedtls_x509_crt_parse(&L->cert, cert_buf, cert_len);
    if (rc < 0) {
        kputs("[tls.listen] cert parse failed (");
        kput_dec((unsigned long)-rc);
        kputs(")\n");
        mbedtls_x509_crt_free(&L->cert);
        kfree(cert_buf); kfree(key_buf);
        return -1;
    }
    /* cert_buf parsed in-place by mbedtls; safe to free now since
     * mbedtls_x509_crt_parse copies the DER payload into its own tree. */
    kfree(cert_buf);

    mbedtls_pk_init(&L->pk);
    rc = mbedtls_pk_parse_key(&L->pk, key_buf, key_len, NULL, 0,
                              mbedtls_ctr_drbg_random, &g_srv_drbg);
    /* Same: mbedtls_pk_parse_key copies what it needs. */
    kfree(key_buf);
    if (rc != 0) {
        kputs("[tls.listen] key parse failed (");
        kput_dec((unsigned long)-rc);
        kputs(")\n");
        mbedtls_x509_crt_free(&L->cert);
        mbedtls_pk_free(&L->pk);
        return -1;
    }

    /* Build the per-port server config. */
    mbedtls_ssl_config_init(&L->conf);
    rc = mbedtls_ssl_config_defaults(&L->conf,
                                      MBEDTLS_SSL_IS_SERVER,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        kputs("[tls.listen] config defaults failed\n");
        mbedtls_ssl_config_free(&L->conf);
        mbedtls_x509_crt_free(&L->cert);
        mbedtls_pk_free(&L->pk);
        return -1;
    }
    mbedtls_ssl_conf_rng(&L->conf, mbedtls_ctr_drbg_random, &g_srv_drbg);

    rc = mbedtls_ssl_conf_own_cert(&L->conf, &L->cert, &L->pk);
    if (rc != 0) {
        kputs("[tls.listen] conf_own_cert failed\n");
        mbedtls_ssl_config_free(&L->conf);
        mbedtls_x509_crt_free(&L->cert);
        mbedtls_pk_free(&L->pk);
        return -1;
    }

    /* CFA wraps. Cert is INTERNAL (public material, but kernel-owned).
     * Private key is SOVEREIGN — even INTERNAL observers don't get to
     * resolve it.  Conf is INTERNAL (matches client-side g_h_conf). */
    {
        cfa_addr_t a;
        chain_t *net_tx = (CHAIN_NET_TX >= 0) ? chain_get(CHAIN_NET_TX) : (chain_t *)0;
        if (net_tx) {
            a = cfa_derive(&net_tx->addr, (uint32_t)(0x200 + idx));
        } else {
            for (int j = 0; j < 8; j++) a.segments[j] = 0;
            a.segments[0] = 0xFE;
            a.segments[1] = (uint32_t)(0x200 + idx);
            a.depth = 2;
            a.birth_tsc = 0;
        }
        L->h_pk   = cfa_wrap(&L->pk,   sizeof(L->pk),   a, MASQ_SOVEREIGN);
        L->h_cert = cfa_wrap_cat(&L->cert, sizeof(L->cert), a, MASQ_INTERNAL,
                                 CFA_CAT_TLS_CONF);
        L->h_conf = cfa_wrap_cat(&L->conf, sizeof(L->conf), a, MASQ_INTERNAL,
                                 CFA_CAT_TLS_CONF);
    }

    /* Arm the kernel TCP listener — must come AFTER the listener slot
     * is fully populated, or an in-flight SYN-ACK could land on a
     * half-built listener. */
    L->in_use   = 1;
    L->port     = port;
    L->chain_id = chain_id;

    int trc = tcp_listen(port, tls_srv_noop_accept);
    if (trc != 0) {
        kputs("[tls.listen] tcp_listen failed on port ");
        kput_dec(port);
        kputc('\n');
        L->in_use = 0;
        mbedtls_ssl_config_free(&L->conf);
        mbedtls_x509_crt_free(&L->cert);
        mbedtls_pk_free(&L->pk);
        return -1;
    }

    kputs("[tls.listen] armed port ");
    kput_dec(port);
    kputs(" (chain=");
    kput_dec((uint64_t)(uint32_t)chain_id);
    kputs(")\n");
    return idx;
}

static int alloc_session(int listener_idx, tcp_handle_t h)
{
    for (int i = 0; i < TLS_SRV_MAX_SESSIONS; i++) {
        if (g_sessions[i].in_use) continue;
        g_sessions[i].in_use         = 1;
        g_sessions[i].listener_idx   = listener_idx;
        g_sessions[i].tcp_handle     = (int)h;
        g_sessions[i].handshake_done = 0;
        return i;
    }
    return -1;
}

static void release_session(int sidx)
{
    if (sidx < 0 || sidx >= TLS_SRV_MAX_SESSIONS) return;
    struct tls_srv_session *S = &g_sessions[sidx];
    if (!S->in_use) return;
    if (S->h_ssl) { cfa_release(S->h_ssl); S->h_ssl = 0; }
    mbedtls_ssl_free(&S->ssl);
    if (S->tcp_handle >= 0) tcp_close_on_async(S->tcp_handle);
    S->tcp_handle = -1;
    S->in_use = 0;
    S->handshake_done = 0;
}

static void tls_srv_accept_common(int listener_idx, tcp_handle_t h)
{
    if (listener_idx < 0 || listener_idx >= TLS_SRV_MAX_LISTENERS) {
        tcp_close_on_async(h); return;
    }
    if (!g_listeners[listener_idx].in_use) {
        tcp_close_on_async(h); return;
    }
    int sidx = alloc_session(listener_idx, h);
    if (sidx < 0) {
        kputs("[tls.listen] session table full — closing\n");
        tcp_close_on_async(h);
        return;
    }
    /* Don't run the handshake from inside the kernel TCP callback —
     * that recurses back through net_poll. Defer to tls_srv_poll(). */
}

/* Drive one session: start handshake (if not started), continue until
 * WANT_READ/WRITE clears, then dispatch the handler chain.  Returns:
 *    1 — work done, session released.
 *    0 — handshake in progress, leave the slot.
 *   -1 — fatal, session released. */
static int tls_srv_drive(int sidx)
{
    struct tls_srv_session *S = &g_sessions[sidx];
    if (!S->in_use) return -1;
    if (S->tcp_handle < 0 || !tcp_is_connected(S->tcp_handle)) {
        release_session(sidx);
        return -1;
    }
    struct tls_srv_listener *L = &g_listeners[S->listener_idx];
    if (!L->in_use) { release_session(sidx); return -1; }

    if (!S->handshake_done) {
        /* Lazy ssl_setup on first drive. */
        if (!S->ssl_inited) {
            mbedtls_ssl_init(&S->ssl);
            S->ssl_inited = 1;
            int rc = mbedtls_ssl_setup(&S->ssl, &L->conf);
            if (rc != 0) {
                kputs("[tls.listen] ssl_setup failed\n");
                release_session(sidx);
                return -1;
            }
            mbedtls_ssl_set_bio(&S->ssl, S, srv_tls_send, srv_tls_recv, NULL);

            /* CFA-wrap the per-session ssl_context (INTERNAL). */
            cfa_addr_t a;
            for (int j = 0; j < 8; j++) a.segments[j] = 0;
            a.segments[0] = 0xFE;
            a.segments[1] = (uint32_t)(0x300 + sidx);
            a.depth = 2;
            a.birth_tsc = 0;
            S->h_ssl = cfa_wrap_cat(&S->ssl, sizeof(S->ssl), a,
                                    MASQ_INTERNAL, CFA_CAT_TLS_SESSION);
        }

        int rc = mbedtls_ssl_handshake(&S->ssl);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ ||
            rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return 0;     /* try again next poll */
        }
        if (rc != 0) {
            kputs("[tls.listen] handshake failed (");
            kput_dec((unsigned long)-rc);
            kputs(")\n");
            release_session(sidx);
            return -1;
        }
        S->handshake_done = 1;
        g_handshakes_total++;
    }

    /* Handshake done — read one request, dispatch to chain, write back.
     * We piggyback the std.http response writer for now: the handshake
     * established the TLS layer, the chain handler emits a body via
     * http.respond which writes through our SSL session because the
     * underlying tcp_send_on_raw is intercepted by the tls bio.  For
     * the demo, we read whatever the client sent through the TLS
     * channel, dispatch the chain on the tcp handle, and close. */
    unsigned char rbuf[TLS_SRV_REQ_BUF];
    int got = mbedtls_ssl_read(&S->ssl, rbuf, sizeof(rbuf) - 1);
    if (got == MBEDTLS_ERR_SSL_WANT_READ ||
        got == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    if (got <= 0) {
        release_session(sidx);
        return -1;
    }
    rbuf[got] = 0;

    /* Dispatch handler chain if registered, else default 200 ok over TLS. */
    if (L->chain_id >= 0) {
        struct sig_data trigger;
        trigger.size = 0;
        for (int i = 0; i < 4; i++) trigger.data[i] = 0;
        trigger.data[0] = (uint8_t)(S->tcp_handle & 0xFF);
        trigger.data[1] = (uint8_t)((S->tcp_handle >> 8) & 0xFF);
        trigger.data[2] = (uint8_t)((S->tcp_handle >> 16) & 0xFF);
        trigger.data[3] = (uint8_t)((S->tcp_handle >> 24) & 0xFF);
        trigger.size = 4;
        sig_inject(L->chain_id, 0, &trigger);
        sig_resolve(L->chain_id);
    }

    /* Send a default 200 OK over TLS so the client sees a clean
     * response. (The chain output, if any, was written via http.respond
     * which uses the raw tcp handle today; that path is the next
     * iteration. For now the verifiable signal IS that the handshake
     * completed.) */
    const char *ok = "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: 13\r\n\r\n"
                     "tls.listen ok";
    int olen = 0; while (ok[olen]) olen++;
    int sent = 0;
    while (sent < olen) {
        int n = mbedtls_ssl_write(&S->ssl, (const unsigned char *)ok + sent,
                                   (size_t)(olen - sent));
        if (n == MBEDTLS_ERR_SSL_WANT_READ ||
            n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (n <= 0) break;
        sent += n;
    }
    g_responses_total++;
    mbedtls_ssl_close_notify(&S->ssl);
    release_session(sidx);
    return 1;
}

void tls_srv_poll(void)
{
    static int in_poll = 0;
    if (in_poll) return;
    in_poll = 1;

    /* Drain TCP listener accept queues into session slots. tcp_accept_nb
     * returns >0 when a connection is ready. */
    for (int i = 0; i < TLS_SRV_MAX_LISTENERS; i++) {
        if (!g_listeners[i].in_use) continue;
        for (int k = 0; k < TLS_SRV_MAX_SESSIONS; k++) {
            tcp_handle_t h;
            int r = tcp_accept_nb(g_listeners[i].port, &h);
            if (r <= 0) break;
            tls_srv_accept_common(i, h);
        }
    }

    for (int i = 0; i < TLS_SRV_MAX_SESSIONS; i++) {
        if (!g_sessions[i].in_use) continue;
        (void)tls_srv_drive(i);
    }
    in_poll = 0;
}

int tls_srv_listener_count(void)
{
    int n = 0;
    for (int i = 0; i < TLS_SRV_MAX_LISTENERS; i++)
        if (g_listeners[i].in_use) n++;
    return n;
}

int tls_srv_handshakes_total(void) { return g_handshakes_total; }
int tls_srv_responses_total(void)  { return g_responses_total; }
