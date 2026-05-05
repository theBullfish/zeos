/*
 * Zeos — std.http (server side)
 */

#include "std_http.h"
#include "net_tcp.h"
#include "kprint.h"

static struct http_listener g_listeners[HTTP_MAX_LISTENERS];
static struct http_route    g_routes   [HTTP_MAX_ROUTES];
static int g_inited = 0;

static int h_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void h_strcpy(char *d, const char *s, int max)
{
    int i; for (i = 0; i < max - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

void zp_http_init(void)
{
    if (g_inited) return;
    for (int i = 0; i < HTTP_MAX_LISTENERS; i++) g_listeners[i].in_use = 0;
    for (int i = 0; i < HTTP_MAX_ROUTES;    i++) g_routes[i].in_use    = 0;
    g_inited = 1;
}

int zp_http_listen(uint16_t port, int chain_id)
{
    zp_http_init();
    /* Replace if same port already registered. */
    for (int i = 0; i < HTTP_MAX_LISTENERS; i++) {
        if (g_listeners[i].in_use && g_listeners[i].port == port) {
            g_listeners[i].chain_id = chain_id;
            return i;
        }
    }
    for (int i = 0; i < HTTP_MAX_LISTENERS; i++) {
        if (!g_listeners[i].in_use) {
            g_listeners[i].in_use = 1;
            g_listeners[i].port = port;
            g_listeners[i].chain_id = chain_id;
            return i;
        }
    }
    return -1;
}

int zp_http_route(const char *method, const char *path, int chain_id)
{
    zp_http_init();
    if (!method || !path) return -1;
    /* Replace if exact match. */
    for (int i = 0; i < HTTP_MAX_ROUTES; i++) {
        if (g_routes[i].in_use &&
            h_streq(g_routes[i].method, method) &&
            h_streq(g_routes[i].path,   path)) {
            g_routes[i].chain_id = chain_id;
            return i;
        }
    }
    for (int i = 0; i < HTTP_MAX_ROUTES; i++) {
        if (!g_routes[i].in_use) {
            g_routes[i].in_use = 1;
            h_strcpy(g_routes[i].method, method, (int)sizeof(g_routes[i].method));
            h_strcpy(g_routes[i].path,   path,   (int)sizeof(g_routes[i].path));
            g_routes[i].chain_id = chain_id;
            return i;
        }
    }
    return -1;
}

int zp_http_respond(int tcp_handle, int status, const char *body, int body_len,
                    const char *content_type)
{
    if (tcp_handle < 0) return -1;
    char hdr[256];
    int n = 0;
    /* Status line. */
    const char *sl = "HTTP/1.1 ";
    while (*sl && n < (int)sizeof(hdr) - 8) hdr[n++] = *sl++;
    /* Status code (3 digits). */
    int s = status;
    if (s < 100 || s > 999) s = 200;
    hdr[n++] = '0' + (s / 100) % 10;
    hdr[n++] = '0' + (s / 10)  % 10;
    hdr[n++] = '0' + (s      ) % 10;
    hdr[n++] = ' ';
    const char *reason = (s >= 200 && s < 300) ? "OK" :
                         (s >= 400 && s < 500) ? "ERR" : "X";
    while (*reason && n < (int)sizeof(hdr) - 4) hdr[n++] = *reason++;
    hdr[n++] = '\r'; hdr[n++] = '\n';
    /* Content-Type. */
    const char *ct = (content_type && *content_type) ? content_type : "text/plain";
    const char *ckey = "Content-Type: ";
    while (*ckey && n < (int)sizeof(hdr) - 4) hdr[n++] = *ckey++;
    while (*ct   && n < (int)sizeof(hdr) - 4) hdr[n++] = *ct++;
    hdr[n++] = '\r'; hdr[n++] = '\n';
    /* Content-Length. */
    const char *lkey = "Content-Length: ";
    while (*lkey && n < (int)sizeof(hdr) - 16) hdr[n++] = *lkey++;
    int bl = body_len;
    if (bl == 0) bl = 0;
    char numbuf[16]; int nb = 0;
    if (bl == 0) numbuf[nb++] = '0';
    int v = bl;
    char tmp[16]; int tn = 0;
    while (v > 0 && tn < 16) { tmp[tn++] = '0' + (v % 10); v /= 10; }
    for (int i = tn - 1; i >= 0; i--) numbuf[nb++] = tmp[i];
    for (int i = 0; i < nb && n < (int)sizeof(hdr) - 4; i++) hdr[n++] = numbuf[i];
    hdr[n++] = '\r'; hdr[n++] = '\n';
    hdr[n++] = '\r'; hdr[n++] = '\n';

    int rc = tcp_send_on(tcp_handle, hdr, (uint16_t)n);
    if (rc < 0) return -1;
    int total = n;
    if (body && body_len > 0) {
        rc = tcp_send_on(tcp_handle, body, (uint16_t)body_len);
        if (rc < 0) return -1;
        total += body_len;
    }
    return total;
}

int zp_http_listener_count(void)
{
    int n = 0;
    for (int i = 0; i < HTTP_MAX_LISTENERS; i++) if (g_listeners[i].in_use) n++;
    return n;
}

int zp_http_route_count(void)
{
    int n = 0;
    for (int i = 0; i < HTTP_MAX_ROUTES; i++) if (g_routes[i].in_use) n++;
    return n;
}

int zp_http_parse_request(const char *buf, int buf_len,
                          char *method, int method_max,
                          char *path,   int path_max)
{
    if (!buf || buf_len <= 0) return -1;
    int i = 0;
    int mn = 0;
    while (i < buf_len && buf[i] != ' ' && mn < method_max - 1) {
        method[mn++] = buf[i++];
    }
    method[mn] = 0;
    if (i >= buf_len || buf[i] != ' ') return -1;
    i++;
    int pn = 0;
    while (i < buf_len && buf[i] != ' ' && buf[i] != '\r' &&
           pn < path_max - 1) {
        path[pn++] = buf[i++];
    }
    path[pn] = 0;
    if (mn == 0 || pn == 0) return -1;
    return 0;
}
