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

#endif /* ZEOS_NET_HTTP_H */
