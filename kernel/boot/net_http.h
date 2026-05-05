/*
 * Zeos — HTTP/1.1 Client
 *
 * GET requests only. Connection: close.
 * The payoff for the entire networking stack.
 */

#ifndef ZEOS_NET_HTTP_H
#define ZEOS_NET_HTTP_H

#include "net.h"

#define HTTP_MAX_BODY  262144   /* 256 KB — fits real-world pages */

struct http_response {
    int      status_code;
    char     content_type[64];
    char     body[HTTP_MAX_BODY];
    uint32_t body_len;            /* uint16 was too narrow at 256 KB */
};

/*
 * Perform an HTTP GET request.
 * host: hostname (will be DNS resolved)
 * path: URL path (e.g., "/", "/index.html")
 * resp: output response struct
 * Returns 0 on success, -1 on failure.
 */
int http_get(const char *host, const char *path, struct http_response *resp);

/*
 * Single-hop HTTP GET. On a 3xx response, redir_loc receives the raw
 * Location header value (NUL-terminated, may be relative). resp still
 * contains the 3xx body. Used internally by http_get to drive redirect
 * chains across schemes.
 */
int http_get_once_loc(const char *host, const char *path,
                      struct http_response *resp,
                      char *redir_loc, int redir_max);

/*
 * Generic HTTP request: forwards an inbound request shape to an upstream
 * URL with the given method (GET / POST / PUT / DELETE / PATCH).
 *
 *   url     — absolute "http://host/path" or "https://host/path"
 *   method  — HTTP verb. NULL or "" defaults to GET.
 *   body    — request body bytes (NULL for none). Sent as-is.
 *   body_len — body length in bytes.
 *   headers — extra request headers, "\r\n"-terminated lines (NULL for
 *             none). Caller is responsible for sane header content.
 *   resp    — output response (status, content_type, body, body_len).
 *
 * Returns 0 on success, -1 on transport failure.
 *
 * GET on a https:// URL goes through https_get (TLS termination, full
 * handshake, redirects). Methods other than GET on https currently
 * hop-out via the v4 path because the TLS-side single-hop helper is
 * GET-only; that's a follow-up. POST-over-TLS through https_get is
 * possible but the helper signature doesn't expose it -- we do the
 * honest thing and return -1 there. POST/PUT/DELETE/PATCH on plain
 * http work end-to-end.
 */
int http_request(const char *url, const char *method,
                 const char *body, int body_len,
                 const char *headers,
                 struct http_response *resp);

#endif /* ZEOS_NET_HTTP_H */
