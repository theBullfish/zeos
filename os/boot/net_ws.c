/*
 * Zeos — WebSocket client (RFC 6455). See net_ws.h.
 *
 * Handshake: HTTP/1.1 Upgrade with a per-connection Sec-WebSocket-Key; the
 * server's Sec-WebSocket-Accept is verified as base64(SHA1(key || GUID)).
 * Frames: client->server masked; text/binary/ping/pong/close.
 */
#include "net_ws.h"
#include "net_dns.h"
#include "kprint.h"
#include "timer.h"

#include "mbedtls/sha1.h"
#include "mbedtls/base64.h"
#include "mbedtls/ssl.h"      /* MBEDTLS_ERR_SSL_WANT_READ/WRITE for wss */

#include <stddef.h>

/* ── tiny local string/mem helpers (freestanding) ─────────────────────── */
static uint32_t ws_strlen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static void ws_memcpy(void *d, const void *s, uint32_t n)
{ uint8_t *dd = d; const uint8_t *ss = s; for (uint32_t i = 0; i < n; i++) dd[i] = ss[i]; }
static int ws_memeq(const void *a, const void *b, uint32_t n)
{ const uint8_t *x = a, *y = b; for (uint32_t i = 0; i < n; i++) if (x[i] != y[i]) return 0; return 1; }
/* case-insensitive substring search over a NUL-free buffer of length hn */
static int ws_find_ci(const char *hay, uint32_t hn, const char *needle)
{
    uint32_t nn = ws_strlen(needle);
    if (nn == 0 || nn > hn) return -1;
    for (uint32_t i = 0; i + nn <= hn; i++) {
        uint32_t j = 0;
        for (; j < nn; j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
        }
        if (j == nn) return (int)i;
    }
    return -1;
}

/* Wall-clock deadline `sec` seconds from now (TSC-based). */
static uint64_t ws_deadline(uint32_t sec)
{
    uint64_t f = timer_tsc_freq();
    if (f == 0) f = 2000000000ULL;
    return timer_read_tsc() + (uint64_t)sec * f;
}
static int ws_expired(uint64_t deadline) { return timer_read_tsc() > deadline; }

/* ── Transport abstraction: ws:// over TCP, wss:// over TLS ────────────── */

/* Send all `len` bytes. Returns 0 on success, -1 on error. */
static int ws_xport_send(ws_conn_t *ws, const uint8_t *data, uint32_t len)
{
    if (ws->secure) {
        uint32_t off = 0;
        while (off < len) {
            int w = tls_send(ws->tls, data + off, (int)(len - off));
            if (w > 0) { off += (uint32_t)w; continue; }
            if (w == MBEDTLS_ERR_SSL_WANT_WRITE || w == MBEDTLS_ERR_SSL_WANT_READ) {
                net_poll(); continue;
            }
            return -1;
        }
        return 0;
    }
    return tcp_send_on(ws->tcp, data, (uint16_t)len) < 0 ? -1 : 0;
}

/* Non-blocking receive: >0 bytes copied, 0 = nothing ready, -1 = closed/error. */
static int ws_xport_recv(ws_conn_t *ws, uint8_t *buf, uint32_t max)
{
    if (ws->secure) {
        int r = tls_recv(ws->tls, buf, (int)max);
        if (r > 0) return r;
        if (r == 0) return -1;                 /* clean EOF = closed */
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
            return 0;
        return -1;
    }
    return tcp_recv_on_nb(ws->tcp, buf, (uint16_t)max);
}

/* Still connected? (best-effort; TLS close surfaces via recv EOF/-1) */
static int ws_xport_connected(ws_conn_t *ws)
{
    if (ws->secure) return ws->tls != 0;
    return tcp_is_connected(ws->tcp);
}

/* Read exactly n bytes into buf over the active transport. Returns 0 on
 * success, -1 on close/error/timeout. Time-based ~6s bound. */
static int ws_read_n(ws_conn_t *ws, uint8_t *buf, uint32_t n)
{
    uint32_t got = 0;
    uint64_t dl = ws_deadline(6);
    while (got < n) {
        int r = ws_xport_recv(ws, buf + got, n - got);
        if (r < 0) return -1;
        if (r == 0) {
            net_poll();               /* pump the stack so incoming frames land */
            if (!ws_xport_connected(ws)) return -1;
            if (ws_expired(dl)) return -1;
            if (!ws->secure) tcp_retransmit_tick();
            continue;
        }
        got += (uint32_t)r;
    }
    return 0;
}

/* Build a 16-byte pseudo-random key, base64-encode into out (<=25 bytes+NUL). */
static void ws_make_key(char *out, uint32_t out_sz)
{
    uint8_t raw[16];
    uint64_t t = timer_read_tsc();
    static uint32_t ctr = 0;
    ctr += 0x9E3779B9u;
    for (int i = 0; i < 16; i++) {
        t = t * 6364136223846793005ULL + 1442695040888963407ULL + ctr;
        raw[i] = (uint8_t)(t >> 33);
    }
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)out, out_sz, &olen, raw, 16);
    if (olen < out_sz) out[olen] = 0; else out[out_sz - 1] = 0;
}

/* Compute the expected Sec-WebSocket-Accept for a given client key. */
static void ws_expected_accept(const char *key, char *out, uint32_t out_sz)
{
    static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t buf[64 + 40];
    uint32_t kl = ws_strlen(key), gl = ws_strlen(GUID);
    if (kl + gl > sizeof(buf)) { out[0] = 0; return; }
    ws_memcpy(buf, key, kl);
    ws_memcpy(buf + kl, GUID, gl);
    uint8_t sha[20];
    mbedtls_sha1(buf, kl + gl, sha);
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)out, out_sz, &olen, sha, 20);
    if (olen < out_sz) out[olen] = 0; else out[out_sz - 1] = 0;
}

/* Send the HTTP Upgrade request over the (already-connected) transport, read
 * the 101 response and verify Sec-WebSocket-Accept. Returns 0 on success. */
static int ws_do_handshake(ws_conn_t *out, const char *host, const char *path)
{
    char key[32];
    ws_make_key(key, sizeof(key));
    char req[512];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, host, key);
    if (n <= 0 || n >= (int)sizeof(req)) return -4;
    if (ws_xport_send(out, (const uint8_t *)req, (uint32_t)n) < 0) return -5;

    /* Read the response headers (until \r\n\r\n). */
    char resp[1024];
    uint32_t rlen = 0;
    int done = 0;
    uint64_t dl = ws_deadline(8);
    while (rlen < sizeof(resp) - 1 && !done) {
        int r = ws_xport_recv(out, (uint8_t *)resp + rlen, sizeof(resp) - 1 - rlen);
        if (r < 0) break;
        if (r == 0) {
            net_poll();
            if (!ws_xport_connected(out) && rlen == 0) break;
            if (ws_expired(dl)) break;
            if (!out->secure) tcp_retransmit_tick();
            continue;
        }
        rlen += (uint32_t)r;
        resp[rlen] = 0;
        if (ws_find_ci(resp, rlen, "\r\n\r\n") >= 0) done = 1;
    }
    if (!done) { kputs("  WS: no handshake response\n"); return -6; }

    if (ws_find_ci(resp, rlen, "101") < 0 ||
        ws_find_ci(resp, rlen, "upgrade: websocket") < 0) {
        kputs("  WS: server did not upgrade (no 101)\n");
        return -7;
    }

    char expect[40];
    ws_expected_accept(key, expect, sizeof(expect));
    int ai = ws_find_ci(resp, rlen, "sec-websocket-accept:");
    if (ai >= 0) {
        const char *p = resp + ai + (int)ws_strlen("sec-websocket-accept:");
        while (*p == ' ') p++;
        uint32_t el = ws_strlen(expect);
        if (!ws_memeq(p, expect, el)) {
            kputs("  WS: Accept mismatch (handshake not trusted)\n");
            return -8;
        }
    }

    out->open = 1;
    kputs(out->secure ? "  WSS: connected (101, over TLS)\n"
                      : "  WS: connected (101 Switching Protocols)\n");
    return 0;
}

int ws_connect(ws_conn_t *out, const char *host, const char *path, uint16_t port)
{
    if (!out || !host) return -1;
    out->open = 0; out->secure = 0; out->tls = 0;
    if (port == 0) port = 80;
    if (!path || !path[0]) path = "/";

    /* Accept a dotted-quad literal (a.b.c.d) directly; else DNS-resolve. */
    struct ipv4_addr ip;
    int oct[4], oi = 0, cur = 0, seen = 0, is_ip = 1;
    for (const char *q = host; ; q++) {
        if (*q >= '0' && *q <= '9') { cur = cur * 10 + (*q - '0'); seen = 1; }
        else if (*q == '.' || *q == 0) {
            if (!seen || cur > 255 || oi >= 4) { is_ip = 0; break; }
            oct[oi++] = cur; cur = 0; seen = 0;
            if (*q == 0) break;
        } else { is_ip = 0; break; }
    }
    if (is_ip && oi == 4) {
        ip.b[0] = (uint8_t)oct[0]; ip.b[1] = (uint8_t)oct[1];
        ip.b[2] = (uint8_t)oct[2]; ip.b[3] = (uint8_t)oct[3];
    } else if (dns_resolve(host, &ip) < 0) {
        kputs("  WS: DNS failed\n");
        return -2;
    }
    tcp_handle_t h = tcp_open(ip, port);
    if (h == TCP_INVALID_HANDLE) {
        kputs("  WS: TCP connect failed\n");
        return -3;
    }
    out->tcp = h;

    int rc = ws_do_handshake(out, host, path);
    if (rc != 0) { tcp_close_on(h); return rc; }
    return 0;
}

int ws_connect_secure(ws_conn_t *out, const char *host, const char *path, uint16_t port)
{
    if (!out || !host) return -1;
    out->open = 0; out->secure = 1; out->tls = 0; out->tcp = TCP_INVALID_HANDLE;
    if (port == 0) port = 443;
    if (!path || !path[0]) path = "/";

    /* tls_connect resolves DNS, does the TLS handshake, and verifies the
     * server cert against the CA bundle (verify REQUIRED). */
    tls_conn_t *t = tls_connect(host, port);
    if (!t) { kputs("  WSS: TLS connect failed\n"); return -3; }
    out->tls = t;

    int rc = ws_do_handshake(out, host, path);
    if (rc != 0) { tls_close(t); out->tls = 0; return rc; }
    return 0;
}

/* Encode + send one masked frame. */
static int ws_send_frame(ws_conn_t *ws, uint8_t opcode, const uint8_t *data, uint32_t len)
{
    if (!ws->open) return -1;
    uint8_t hdr[14];
    uint32_t h = 0;
    hdr[h++] = (uint8_t)(0x80 | (opcode & 0x0F));   /* FIN + opcode */
    if (len < 126) {
        hdr[h++] = (uint8_t)(0x80 | len);           /* MASK + len */
    } else if (len <= 0xFFFF) {
        hdr[h++] = (uint8_t)(0x80 | 126);
        hdr[h++] = (uint8_t)(len >> 8);
        hdr[h++] = (uint8_t)(len & 0xFF);
    } else {
        return -1;   /* > 64KiB frames unsupported */
    }
    /* Masking key. */
    uint64_t t = timer_read_tsc();
    uint8_t mask[4] = { (uint8_t)t, (uint8_t)(t >> 8), (uint8_t)(t >> 16), (uint8_t)(t >> 24) };
    ws_memcpy(hdr + h, mask, 4); h += 4;

    if (ws_xport_send(ws, hdr, h) < 0) return -1;

    /* Masked payload, in chunks. */
    uint8_t chunk[512];
    uint32_t off = 0;
    while (off < len) {
        uint32_t c = len - off; if (c > sizeof(chunk)) c = sizeof(chunk);
        for (uint32_t i = 0; i < c; i++)
            chunk[i] = (uint8_t)(data[off + i] ^ mask[(off + i) & 3]);
        if (ws_xport_send(ws, chunk, c) < 0) return -1;
        off += c;
    }
    return 0;
}

int ws_send_text(ws_conn_t *ws, const char *data, uint32_t len)
{
    return ws_send_frame(ws, WS_OP_TEXT, (const uint8_t *)data, len);
}

int ws_recv(ws_conn_t *ws, void *buf, uint32_t max_len, int *opcode_out)
{
    if (!ws->open) return -1;
    for (int guard = 0; guard < 8; guard++) {
        uint8_t h2[2];
        if (ws_read_n(ws, h2, 2) < 0) return -1;
        uint8_t opcode = h2[0] & 0x0F;
        int masked = (h2[1] & 0x80) != 0;
        uint32_t len = h2[1] & 0x7F;
        if (len == 126) {
            uint8_t ext[2];
            if (ws_read_n(ws, ext, 2) < 0) return -1;
            len = ((uint32_t)ext[0] << 8) | ext[1];
        } else if (len == 127) {
            return -1;   /* 64-bit lengths unsupported */
        }
        uint8_t mask[4] = {0,0,0,0};
        if (masked && ws_read_n(ws, mask, 4) < 0) return -1;

        if (len > WS_MAX_FRAME) return -1;
        if (len && ws_read_n(ws, ws->rx, len) < 0) return -1;
        if (masked)
            for (uint32_t i = 0; i < len; i++) ws->rx[i] ^= mask[i & 3];

        if (opcode == WS_OP_PING) {          /* answer ping, keep reading */
            ws_send_frame(ws, WS_OP_PONG, ws->rx, len);
            continue;
        }
        if (opcode == WS_OP_PONG) continue;
        if (opcode == WS_OP_CLOSE) { ws->open = 0; return -1; }

        /* data frame (text/binary/cont) */
        uint32_t c = len; if (c > max_len) c = max_len;
        ws_memcpy(buf, ws->rx, c);
        if (opcode_out) *opcode_out = opcode;
        return (int)c;
    }
    return -1;   /* too many control frames in a row */
}

void ws_close(ws_conn_t *ws)
{
    if (ws->open) {
        uint8_t empty = 0;
        ws_send_frame(ws, WS_OP_CLOSE, &empty, 0);
        ws->open = 0;
    }
    if (ws->secure) {
        if (ws->tls) { tls_close(ws->tls); ws->tls = 0; }
    } else {
        tcp_close_on(ws->tcp);
    }
}
