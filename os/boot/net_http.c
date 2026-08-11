/*
 * Zeos — HTTP/1.1 Client
 *
 * url -> dns_resolve -> tcp_connect -> send_request -> receive_response
 * That's a signal chain. This is just the imperative version until
 * the signal chain engine supports async I/O.
 */

#include "net_http.h"
#include "net_tcp.h"
#include "net_tcp6.h"
#include "net_dns.h"
#include "net_ipv6.h"
#include "net_tls.h"
#include "kprint.h"

#define HTTP_MAX_REDIRECTS 5

static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void mem_set(void *dst, uint8_t v, int n) {
    uint8_t *d = (uint8_t *)dst;
    for (int i = 0; i < n; i++) d[i] = v;
}

static int str_starts_ci(const char *s, const char *prefix) {
    while (*prefix) {
        char a = *s++, b = *prefix++;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

/*
 * Resolve a Location: header value against an originating (scheme, host, path).
 * out_url receives the absolute URL (max-1 bytes + NUL).
 * Handles:
 *   - absolute http:// or https:// URLs (used as-is)
 *   - protocol-relative //host/path (inherits scheme)
 *   - root-absolute /path (inherits scheme + host)
 *   - relative path (resolved against directory of base path)
 */
static void resolve_location(int base_use_tls, const char *base_host,
                             const char *base_path, const char *loc,
                             char *out_url, int max)
{
    int o = 0;
    if (str_starts_ci(loc, "http://") || str_starts_ci(loc, "https://")) {
        while (*loc && o < max - 1) out_url[o++] = *loc++;
        out_url[o] = 0;
        return;
    }
    const char *scheme = base_use_tls ? "https://" : "http://";
    while (*scheme && o < max - 1) out_url[o++] = *scheme++;

    if (loc[0] == '/' && loc[1] == '/') {
        /* protocol-relative */
        loc += 2;
        while (*loc && o < max - 1) out_url[o++] = *loc++;
        out_url[o] = 0;
        return;
    }

    while (*base_host && o < max - 1) out_url[o++] = *base_host++;

    if (loc[0] == '/') {
        while (*loc && o < max - 1) out_url[o++] = *loc++;
    } else {
        /* relative — copy base_path up to last '/' */
        int last_slash = 0;
        for (int i = 0; base_path[i]; i++) if (base_path[i] == '/') last_slash = i;
        for (int i = 0; i <= last_slash && o < max - 1; i++) out_url[o++] = base_path[i];
        while (*loc && o < max - 1) out_url[o++] = *loc++;
    }
    out_url[o] = 0;
}

/* Split absolute URL → use_tls, host, path. */
static void split_url(const char *url, int *use_tls,
                      char *host, int hmax, char *path, int pmax)
{
    *use_tls = 0;
    const char *p = url;
    if (str_starts_ci(p, "https://")) { *use_tls = 1; p += 8; }
    else if (str_starts_ci(p, "http://")) { p += 7; }

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

/* Find a substring in a buffer (case-insensitive for header names) */
static const char *find_header(const char *buf, int len, const char *name)
{
    int nlen = str_len(name);
    for (int i = 0; i < len - nlen; i++) {
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            char a = buf[i + j];
            char b = name[j];
            /* Case-insensitive compare */
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = 0; break; }
        }
        if (match)
            return buf + i + nlen;
    }
    return 0;
}

/* Find \r\n\r\n (end of headers) */
static int find_body_start(const char *buf, int len)
{
    for (int i = 0; i < len - 3; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n')
            return i + 4;
    }
    return -1;
}

/* Find Location header value into out (NUL-terminated). Returns 1 if found.
 * HTTP responses always have a status line first, so Location: never appears
 * at offset 0 — we only need the "\nLocation: " form. */
static int find_location(const char *raw, int total, char *out, int max)
{
    const char *loc = find_header(raw, total, "\nlocation: ");
    if (!loc) return 0;
    int i = 0;
    while (*loc == ' ' || *loc == '\t') loc++;
    while (*loc && *loc != '\r' && *loc != '\n' && i < max - 1) out[i++] = *loc++;
    out[i] = 0;
    return i > 0;
}

/* Forward decl — implemented in net_tls.c. Returns status or -1; on 3xx fills
 * redir_loc with the raw Location header value. */
extern int https_get_once(const char *hostname, const char *path,
                          char *resp_buf, int resp_max, int *body_len,
                          char *redir_loc, int redir_max);

int http_get_once_loc(const char *host, const char *path,
                      struct http_response *resp,
                      char *redir_loc, int redir_max);

int http_get(const char *host, const char *path, struct http_response *resp)
{
    /* Static — only one HTTP request in flight at a time, and these would
     * blow the UEFI stack if we put them on it. */
    static char cur_host[256];
    static char cur_path[1024];
    int  cur_tls = 0;

    int i = 0;
    while (host[i] && i < 255) { cur_host[i] = host[i]; i++; }
    cur_host[i] = 0;
    i = 0;
    while (path[i] && i < 1023) { cur_path[i] = path[i]; i++; }
    cur_path[i] = 0;

    for (int hop = 0; hop <= HTTP_MAX_REDIRECTS; hop++) {
        static char redir[2048]; redir[0] = 0;
        int status, body_len = 0;

        if (cur_tls) {
            static char tls_buf[HTTP_MAX_BODY + 2048];
            status = https_get_once(cur_host, cur_path,
                                    tls_buf, sizeof(tls_buf), &body_len,
                                    redir, sizeof(redir));
            if (status < 0) return -1;

            if ((status == 301 || status == 302 || status == 307 || status == 308) &&
                redir[0] && hop < HTTP_MAX_REDIRECTS) {
                static char absu[2048];
                resolve_location(1, cur_host, cur_path, redir, absu, sizeof(absu));
                kputs("  HTTP: redirect -> "); kputs(absu); kputs("\n");
                split_url(absu, &cur_tls, cur_host, 256, cur_path, 1024);
                continue;
            }

            mem_set(resp, 0, sizeof(*resp));
            resp->status_code = status;
            int copy = body_len;
            if (copy > HTTP_MAX_BODY - 1) copy = HTTP_MAX_BODY - 1;
            for (int k = 0; k < copy; k++) resp->body[k] = tls_buf[k];
            resp->body[copy] = 0;
            resp->body_len = (uint32_t)copy;
            return 0;
        }

        int rc = http_get_once_loc(cur_host, cur_path, resp, redir, sizeof(redir));
        if (rc < 0) return -1;

        int sc = resp->status_code;
        if ((sc == 301 || sc == 302 || sc == 307 || sc == 308) &&
            redir[0] && hop < HTTP_MAX_REDIRECTS) {
            char absu[2048];
            resolve_location(0, cur_host, cur_path, redir, absu, sizeof(absu));
            kputs("  HTTP: redirect -> "); kputs(absu); kputs("\n");
            split_url(absu, &cur_tls, cur_host, 256, cur_path, 1024);
            continue;
        }
        return 0;
    }

    kputs("  HTTP: redirect limit reached\n");
    return -1;
}

int http_get_once_loc(const char *host, const char *path,
                      struct http_response *resp,
                      char *redir_loc, int redir_max)
{
    mem_set(resp, 0, sizeof(*resp));
    if (redir_max > 0) redir_loc[0] = 0;

    /* Happy Eyeballs (simplified): resolve AAAA + A in series, try v6
     * connect first. If v6 connect fails (or AAAA missing) we fall back
     * to v4. The 200 ms threshold from RFC 8305 is implicit: tcp_open_v6
     * blocks at most 5 s waiting for SYN-ACK; we either get one quickly
     * or fall through. The structure stays single-threaded in this kernel. */
    kputs("  DNS: resolving ");
    kputs(host);
    kputs("...\n");

    int via_v6 = 0;
    tcp_handle_t v6h = TCP_INVALID_HANDLE;
    struct ipv6_addr server_ip6;
    if (dns_resolve_aaaa(host, &server_ip6) == 0) {
        char buf[48];
        ipv6_format(server_ip6, buf);
        kputs("  DNS6: "); kputs(buf); kputs("\n");
        kputs("  TCP6: connecting...\n");
        v6h = tcp_open_v6(server_ip6, 80);
        if (v6h != TCP_INVALID_HANDLE) {
            via_v6 = 1;
            kputs("  TCP6: connected\n");
        } else {
            kputs("  TCP6: failed -- falling back to v4\n");
        }
    }

    struct ipv4_addr server_ip;
    struct tcp_conn conn;
    if (!via_v6) {
        if (dns_resolve(host, &server_ip) < 0) {
            kputs("  DNS: failed\n");
            return -1;
        }
        kputs("  DNS: ");
        kput_dec(server_ip.b[0]); kputs(".");
        kput_dec(server_ip.b[1]); kputs(".");
        kput_dec(server_ip.b[2]); kputs(".");
        kput_dec(server_ip.b[3]); kputs("\n");
        kputs("  TCP: connecting...\n");
        if (tcp_connect(&conn, server_ip, 80) < 0) {
            kputs("  TCP: connect failed\n");
            return -1;
        }
        kputs("  TCP: connected\n");
    }

    /* Build HTTP request */
    char request[512];
    int rpos = 0;

    /* GET /path HTTP/1.1\r\n */
    const char *g = "GET ";
    while (*g) request[rpos++] = *g++;
    while (*path) request[rpos++] = *path++;
    const char *v = " HTTP/1.1\r\n";
    while (*v) request[rpos++] = *v++;

    /* Host: header */
    const char *hh = "Host: ";
    while (*hh) request[rpos++] = *hh++;
    const char *h = host;
    while (*h) request[rpos++] = *h++;
    request[rpos++] = '\r'; request[rpos++] = '\n';

    /* Connection: close */
    const char *cc = "Connection: close\r\n";
    while (*cc) request[rpos++] = *cc++;

    /* User-Agent */
    const char *ua = "User-Agent: Surf/1.0 (Zeos)\r\n";
    while (*ua) request[rpos++] = *ua++;

    /* End of headers */
    request[rpos++] = '\r'; request[rpos++] = '\n';

    /* Send */
    kputs("  HTTP: sending request...\n");
    if (via_v6) tcp_send_v6(v6h, request, (uint16_t)rpos);
    else        tcp_send(&conn, request, (uint16_t)rpos);

    /* Receive response — keep buffer in BSS (256 KB+ won't fit on UEFI stack).
     * Chunk reads to a uint16_t-bounded size since tcp_recv's len arg is u16. */
    kputs("  HTTP: receiving...\n");
    static char raw[HTTP_MAX_BODY + 2048];
    int total = 0;
    int cap = (int)sizeof(raw) - 1;

    while (total < cap) {
        int remaining = cap - total;
        uint16_t chunk = remaining > 0xF000 ? 0xF000 : (uint16_t)remaining;
        int got = via_v6 ? tcp_recv_v6(v6h, raw + total, chunk)
                         : tcp_recv(&conn, raw + total, chunk);
        if (got <= 0) break;
        total += got;
    }
    raw[total] = '\0';

    if (via_v6) tcp_close_v6(v6h);
    else        tcp_close(&conn);

    if (total == 0) {
        kputs("  HTTP: no response\n");
        return -1;
    }

    /* Parse status code: require "HTTP/x.y NNN" with digits at 9-11. */
    if (total >= 12 && raw[0] == 'H' && raw[1] == 'T' && raw[2] == 'T' && raw[3] == 'P' &&
        raw[9]  >= '0' && raw[9]  <= '9' &&
        raw[10] >= '0' && raw[10] <= '9' &&
        raw[11] >= '0' && raw[11] <= '9') {
        resp->status_code = (raw[9] - '0') * 100 + (raw[10] - '0') * 10 + (raw[11] - '0');
    }

    /* Parse Content-Type */
    const char *ct = find_header(raw, total, "content-type: ");
    if (ct) {
        int ci = 0;
        while (*ct && *ct != '\r' && *ct != '\n' && ci < 63)
            resp->content_type[ci++] = *ct++;
        resp->content_type[ci] = '\0';
    }

    /* Parse Location for 3xx */
    if (redir_max > 0) {
        find_location(raw, total, redir_loc, redir_max);
    }

    /* Find body */
    int body_start = find_body_start(raw, total);
    if (body_start > 0) {
        int body_len = total - body_start;
        if (body_len > HTTP_MAX_BODY - 1)
            body_len = HTTP_MAX_BODY - 1;
        for (int i = 0; i < body_len; i++)
            resp->body[i] = raw[body_start + i];
        resp->body[body_len] = '\0';
        resp->body_len = (uint32_t)body_len;
    }

    kputs("  HTTP: ");
    kput_dec(resp->status_code);
    kputs(" (");
    kput_dec(resp->body_len);
    kputs(" bytes)\n");

    return 0;
}

/* ── Generic outbound request (GET/POST/PUT/DELETE/PATCH) ───────────
 * Reuses the dual-stack TCP path used by http_get_once_loc, but we
 * don't share the redirect-chasing buffer/state because the verb is
 * called from inside the Z+ proc which already runs on the request
 * path; one redirect hop is enough for the proxy use-case. */

static int hr_strlen(const char *s) { int n = 0; if (!s) return 0; while (s[n]) n++; return n; }
static int hr_streq_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == 0 && *b == 0;
}

int http_request(const char *url, const char *method,
                 const char *body, int body_len,
                 const char *headers,
                 struct http_response *resp)
{
    if (!url || !*url || !resp) return -1;
    /* Zero out resp up front. */
    {
        unsigned char *r = (unsigned char *)resp;
        for (unsigned i = 0; i < sizeof(*resp); i++) r[i] = 0;
    }

    /* GET fast-path: defer to http_get / https_get for redirect chasing. */
    const char *m = (method && *method) ? method : "GET";
    if (hr_streq_ci(m, "GET")) {
        char host[256], path[1024];
        int use_tls = 0;
        split_url(url, &use_tls, host, sizeof(host), path, sizeof(path));
        if (use_tls) {
            static char tls_buf[HTTP_MAX_BODY + 2048];
            int blen = 0;
            int status = https_get(host, path, tls_buf, sizeof(tls_buf), &blen);
            if (status < 0) return -1;
            resp->status_code = status;
            int copy = blen;
            if (copy > HTTP_MAX_BODY - 1) copy = HTTP_MAX_BODY - 1;
            for (int k = 0; k < copy; k++) resp->body[k] = tls_buf[k];
            resp->body[copy] = 0;
            resp->body_len = (uint32_t)copy;
            return 0;
        }
        return http_get(host, path, resp);
    }

    /* Non-GET: parse, connect, send method+body, read response. */
    char host[256], path[1024];
    int use_tls = 0;
    split_url(url, &use_tls, host, sizeof(host), path, sizeof(path));
    if (use_tls) {
        kputs("  HTTP: ");
        kputs(m);
        kputs(" over TLS not yet supported (GET works); use http://\n");
        return -1;
    }

    /* v4-only fast path -- the dual-stack happy-eyeballs lives in
     * http_get_once_loc; non-GET is uncommon enough that v4-only is
     * the right tradeoff today. */
    struct ipv4_addr server_ip;
    if (dns_resolve(host, &server_ip) < 0) return -1;

    struct tcp_conn conn;
    if (tcp_connect(&conn, server_ip, 80) < 0) return -1;

    /* Request line + headers. Static buffer because UEFI stack budgets. */
    static char request[4096];
    int rpos = 0;
    for (int i = 0; m[i]; i++) request[rpos++] = m[i];
    request[rpos++] = ' ';
    for (int i = 0; path[i]; i++) request[rpos++] = path[i];
    const char *vhdr = " HTTP/1.1\r\nHost: ";
    for (int i = 0; vhdr[i]; i++) request[rpos++] = vhdr[i];
    for (int i = 0; host[i]; i++) request[rpos++] = host[i];
    const char *cclose = "\r\nConnection: close\r\n"
                         "User-Agent: Zeos/0.1\r\n";
    for (int i = 0; cclose[i]; i++) request[rpos++] = cclose[i];

    /* Optional caller-supplied extra headers. Caller responsible for
     * \r\n separation. */
    if (headers && *headers) {
        for (int i = 0; headers[i] && rpos < (int)sizeof(request) - 64; i++)
            request[rpos++] = headers[i];
    }

    /* Body framing: if body present and no caller-supplied
     * Content-Length, add one. Default Content-Type only if needed. */
    int has_body = body && body_len > 0;
    if (has_body) {
        const char *cl = "Content-Length: ";
        for (int i = 0; cl[i]; i++) request[rpos++] = cl[i];
        char num[16]; int nb = 0; int v = body_len;
        if (v == 0) num[nb++] = '0';
        char tmp[16]; int tn = 0;
        while (v > 0) { tmp[tn++] = '0' + (v % 10); v /= 10; }
        for (int i = tn - 1; i >= 0; i--) num[nb++] = tmp[i];
        for (int i = 0; i < nb; i++) request[rpos++] = num[i];
        request[rpos++] = '\r'; request[rpos++] = '\n';
    }
    request[rpos++] = '\r'; request[rpos++] = '\n';

    if (tcp_send(&conn, request, (uint16_t)rpos) < 0) {
        tcp_close(&conn);
        return -1;
    }
    if (has_body) {
        int sent = 0;
        while (sent < body_len) {
            int chunk = body_len - sent;
            if (chunk > 0xF000) chunk = 0xF000;
            int n = tcp_send(&conn, body + sent, (uint16_t)chunk);
            if (n <= 0) break;
            sent += n;
        }
    }

    /* Receive response. */
    static char raw[HTTP_MAX_BODY + 2048];
    int total = 0;
    int cap = (int)sizeof(raw) - 1;
    while (total < cap) {
        int remaining = cap - total;
        uint16_t chunk = remaining > 0xF000 ? 0xF000 : (uint16_t)remaining;
        int got = tcp_recv(&conn, raw + total, chunk);
        if (got <= 0) break;
        total += got;
    }
    raw[total] = 0;
    tcp_close(&conn);

    if (total < 12) return -1;
    if (raw[9] >= '0' && raw[9] <= '9' &&
        raw[10] >= '0' && raw[10] <= '9' &&
        raw[11] >= '0' && raw[11] <= '9') {
        resp->status_code = (raw[9] - '0') * 100 + (raw[10] - '0') * 10 + (raw[11] - '0');
    }

    const char *ct = find_header(raw, total, "content-type: ");
    if (ct) {
        int ci = 0;
        while (*ct && *ct != '\r' && *ct != '\n' && ci < 63)
            resp->content_type[ci++] = *ct++;
        resp->content_type[ci] = 0;
    }

    int body_start = find_body_start(raw, total);
    if (body_start > 0) {
        int blen = total - body_start;
        if (blen > HTTP_MAX_BODY - 1) blen = HTTP_MAX_BODY - 1;
        for (int i = 0; i < blen; i++) resp->body[i] = raw[body_start + i];
        resp->body[blen] = 0;
        resp->body_len = (uint32_t)blen;
    }
    (void)hr_strlen;
    return 0;
}
