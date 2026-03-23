/*
 * Zeos — HTTP/1.1 Client
 *
 * url -> dns_resolve -> tcp_connect -> send_request -> receive_response
 * That's a signal chain. This is just the imperative version until
 * the signal chain engine supports async I/O.
 */

#include "net_http.h"
#include "net_tcp.h"
#include "net_dns.h"
#include "kprint.h"

static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void mem_set(void *dst, uint8_t v, int n) {
    uint8_t *d = (uint8_t *)dst;
    for (int i = 0; i < n; i++) d[i] = v;
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

int http_get(const char *host, const char *path, struct http_response *resp)
{
    mem_set(resp, 0, sizeof(*resp));

    /* DNS resolve */
    struct ipv4_addr server_ip;
    kputs("  DNS: resolving ");
    kputs(host);
    kputs("...\n");

    if (dns_resolve(host, &server_ip) < 0) {
        kputs("  DNS: failed\n");
        return -1;
    }

    kputs("  DNS: ");
    kput_dec(server_ip.b[0]); kputs(".");
    kput_dec(server_ip.b[1]); kputs(".");
    kput_dec(server_ip.b[2]); kputs(".");
    kput_dec(server_ip.b[3]); kputs("\n");

    /* TCP connect */
    struct tcp_conn conn;
    kputs("  TCP: connecting...\n");

    if (tcp_connect(&conn, server_ip, 80) < 0) {
        kputs("  TCP: connect failed\n");
        return -1;
    }
    kputs("  TCP: connected\n");

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
    tcp_send(&conn, request, (uint16_t)rpos);

    /* Receive response */
    kputs("  HTTP: receiving...\n");
    char raw[HTTP_MAX_BODY + 2048];  /* Headers + body */
    int total = 0;

    while (total < (int)sizeof(raw) - 1) {
        int got = tcp_recv(&conn, raw + total, (uint16_t)(sizeof(raw) - 1 - total));
        if (got <= 0) break;
        total += got;
    }
    raw[total] = '\0';

    tcp_close(&conn);

    if (total == 0) {
        kputs("  HTTP: no response\n");
        return -1;
    }

    /* Parse status code */
    /* HTTP/1.x NNN */
    if (total > 12 && raw[0] == 'H' && raw[1] == 'T' && raw[2] == 'T' && raw[3] == 'P') {
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

    /* Find body */
    int body_start = find_body_start(raw, total);
    if (body_start > 0) {
        int body_len = total - body_start;
        if (body_len > HTTP_MAX_BODY - 1)
            body_len = HTTP_MAX_BODY - 1;
        for (int i = 0; i < body_len; i++)
            resp->body[i] = raw[body_start + i];
        resp->body[body_len] = '\0';
        resp->body_len = (uint16_t)body_len;
    }

    kputs("  HTTP: ");
    kput_dec(resp->status_code);
    kputs(" (");
    kput_dec(resp->body_len);
    kputs(" bytes)\n");

    return 0;
}
