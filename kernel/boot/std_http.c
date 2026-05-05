/*
 * Zeos — std.http (server side)
 */

#include "std_http.h"
#include "net_tcp.h"
#include "signal.h"
#include "kprint.h"

static struct http_listener g_listeners[HTTP_MAX_LISTENERS];
static struct http_route    g_routes   [HTTP_MAX_ROUTES];
static int g_inited = 0;

/* HTTP-owned accepted connections.  Populated by the accept callback,
 * drained by zp_http_poll().  Each entry is a tcp_handle_t (>=0). */
#define HTTP_OWNED_MAX  16
static int g_owned[HTTP_OWNED_MAX];
static int g_accepted_total = 0;
static int g_responses_total = 0;

static void owned_init_once(void)
{
    static int once = 0;
    if (once) return;
    for (int i = 0; i < HTTP_OWNED_MAX; i++) g_owned[i] = -1;
    once = 1;
}

static void owned_add(int h)
{
    owned_init_once();
    for (int i = 0; i < HTTP_OWNED_MAX; i++) {
        if (g_owned[i] == -1) { g_owned[i] = h; return; }
    }
    /* Owned table full — close the connection so we don't leak. */
    tcp_close_on_async(h);
}

/* Accept callback — runs from inside tcp_process(). Keep it tiny. */
static void zp_http_accept_cb(tcp_handle_t h)
{
    g_accepted_total++;
    owned_add((int)h);
}

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
    owned_init_once();

    int idx = -1;
    /* Replace if same port already registered. */
    for (int i = 0; i < HTTP_MAX_LISTENERS; i++) {
        if (g_listeners[i].in_use && g_listeners[i].port == port) {
            g_listeners[i].chain_id = chain_id;
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        for (int i = 0; i < HTTP_MAX_LISTENERS; i++) {
            if (!g_listeners[i].in_use) {
                g_listeners[i].in_use = 1;
                g_listeners[i].port = port;
                g_listeners[i].chain_id = chain_id;
                idx = i;
                break;
            }
        }
    }
    if (idx < 0) return -1;

    /* Arm the kernel TCP listener so real SYNs get answered. */
    int rc = tcp_listen(port, zp_http_accept_cb);
    if (rc != 0) {
        kputs("[http] tcp_listen failed on port ");
        kput_dec(port);
        kputc('\n');
    }
    return idx;
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

    int rc = tcp_send_on_raw(tcp_handle, hdr, (uint16_t)n);
    if (rc < 0) return -1;
    int total = n;
    if (body && body_len > 0) {
        rc = tcp_send_on_raw(tcp_handle, body, (uint16_t)body_len);
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

/* ── Accept-drain poll ───────────────────────── */

/* Find a route whose method+path match exactly. Returns chain_id or -1. */
static int zp_http_route_lookup(const char *method, const char *path)
{
    for (int i = 0; i < HTTP_MAX_ROUTES; i++) {
        if (!g_routes[i].in_use) continue;
        if (h_streq(g_routes[i].method, method) &&
            h_streq(g_routes[i].path,   path))
            return g_routes[i].chain_id;
    }
    return -1;
}

/* Try to dispatch a single HTTP-owned handle.  Returns:
 *   1  request handled (response sent, connection closed)
 *   0  no full request yet (still buffering)
 *  -1  connection died / parse error (caller should remove from owned) */
static int zp_http_drive_handle(int h)
{
    /* Pull any queued bytes from the connection's rx buffer (non-blocking). */
    char buf[HTTP_REQ_BUF];
    int got = tcp_recv_on_nb(h, buf, (uint16_t)(sizeof(buf) - 1));
    if (got <= 0) {
        /* Either remote closed or no data this poll. tcp_recv_on returns 0
         * on close+empty and on timeout; conservatively treat 0 as "no data
         * yet" so a slow client gets another tick. The owned slot is GC'd
         * when the handle leaves ESTABLISHED (checked by the poll loop). */
        return 0;
    }
    buf[got] = 0;

    /* Need the full request line + headers terminator (\r\n\r\n) to be
     * confident we have a parseable request. The kernel parser only
     * needs method+path, so a single \r\n suffices for our minimal
     * server, but we still wait for the empty-line terminator to match
     * curl behavior. */
    int has_terminator = 0;
    for (int i = 0; i + 3 < got; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n') {
            has_terminator = 1; break;
        }
    }
    if (!has_terminator) {
        /* Only a partial request — not enough to parse confidently. */
        return 0;
    }

    char method[8] = {0};
    char path  [64] = {0};
    if (zp_http_parse_request(buf, got, method, (int)sizeof(method),
                               path, (int)sizeof(path)) != 0) {
        /* Malformed: send 400 then close. */
        zp_http_respond(h, 400, "bad request\n", 12, "text/plain");
        g_responses_total++;
        tcp_close_on_async(h);
        return -1;
    }

    /* Find which listener owns this handle (by local_port).  Kernel
     * connection table is opaque from here, so we walk the listener
     * record using a port lookup that mirrors the conn's port. The
     * tcp_handle_t IS the slot index; the kernel exposes local_port
     * through tcp_is_connected style probes — we read port indirectly
     * by trying every listener and dispatching the first one whose
     * route table matches.  In practice listeners are 1–2 entries. */
    int chain_id = -1;

    /* Route table override (method+path exact match). */
    int rc_id = zp_http_route_lookup(method, path);
    if (rc_id >= 0) chain_id = rc_id;

    if (chain_id < 0) {
        /* Fallback: first listener record. (We don't have a public
         * accessor for the conn's local port; the listener table is
         * walked here so a single-port server still works deterministically.
         * Multi-port servers should populate the route table.) */
        for (int i = 0; i < HTTP_MAX_LISTENERS; i++) {
            if (g_listeners[i].in_use) {
                chain_id = g_listeners[i].chain_id;
                break;
            }
        }
    }

    if (chain_id >= 0) {
        struct sig_data trigger;
        trigger.size = 0;
        sig_data_write_i32(&trigger, h);
        /* Inject into node 0; runtime walks the chain and any
         * http.respond reads the handle off the wire. */
        sig_inject(chain_id, 0, &trigger);
        sig_resolve(chain_id);
        g_responses_total++;
    } else {
        /* No handler chain — send a default 200 ok body. */
        const char *body = "ok\n";
        zp_http_respond(h, 200, body, 3, "text/plain");
        g_responses_total++;
    }

    tcp_close_on_async(h);
    return 1;
}

void zp_http_poll(void)
{
    static int in_poll = 0;
    if (in_poll) return;          /* re-entry from tcp_send_on/net_poll */
    in_poll = 1;

    owned_init_once();
    for (int i = 0; i < HTTP_OWNED_MAX; i++) {
        int h = g_owned[i];
        if (h < 0) continue;

        /* Drop entries that lost their connection. */
        if (!tcp_is_connected(h)) {
            g_owned[i] = -1;
            continue;
        }

        int rc = zp_http_drive_handle(h);
        if (rc != 0) {
            /* Handled or errored — release the slot. */
            g_owned[i] = -1;
        }
    }
    in_poll = 0;
}

int zp_http_accepted_total(void)  { return g_accepted_total; }
int zp_http_responses_total(void) { return g_responses_total; }
