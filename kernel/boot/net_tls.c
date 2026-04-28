/*
 * Zeos — TLS Layer
 *
 * Stub implementation. The mbedTLS sources are vendored at
 * kernel/lib/mbedtls/ (3.6 LTS) but are NOT yet compiled into
 * the kernel — kernel/Makefile SRCS does not include any
 * lib/mbedtls/library/*.c files. Wiring those in (and providing
 * the freestanding libc shims they need) is a separate task.
 *
 * Until then: tls_init() succeeds, all I/O calls return -1,
 * and https_get() falls through gracefully. This file gets
 * replaced with the live integration once mbedTLS is actually
 * linked.
 */

#include "net_tls.h"
#include "net_tcp.h"
#include "net_dns.h"
#include "kprint.h"

/* ── TLS connection structure (stub) ── */

struct tls_conn {
    int active;
    char hostname[256];
    uint16_t port;
};

static struct tls_conn g_tls;

/* ── Public API (stubs until mbedTLS is linked) ── */

int tls_init(void)
{
    kputs("TLS: stub mode (mbedTLS vendored but not linked)\n");
    return 0;
}

tls_conn_t *tls_connect(const char *hostname, uint16_t port)
{
    (void)hostname; (void)port;
    kputs("TLS: connect unavailable (stub)\n");
    return 0;
}

int tls_send(tls_conn_t *conn, const void *data, int len)
{
    (void)conn; (void)data; (void)len;
    return -1;
}

int tls_recv(tls_conn_t *conn, void *buf, int max_len)
{
    (void)conn; (void)buf; (void)max_len;
    return -1;
}

void tls_close(tls_conn_t *conn)
{
    if (conn) conn->active = 0;
}

/* ── HTTPS convenience ── */

extern int str_copy(char *dst, const char *src);
extern int str_len(const char *s);

int https_get(const char *hostname, const char *path,
              char *resp_buf, int resp_max, int *body_len)
{
    if (!hostname || !path || !resp_buf || resp_max < 16) return -1;

    kputs("HTTPS: GET https://");
    kputs(hostname);
    kputs(path);
    kputs("\n");

    tls_conn_t *conn = tls_connect(hostname, 443);
    if (!conn) return -1;

    char req[1024];
    req[0] = 0;
    str_copy(req, "GET ");
    str_copy(req + str_len(req), path);
    str_copy(req + str_len(req), " HTTP/1.1\r\nHost: ");
    str_copy(req + str_len(req), hostname);
    str_copy(req + str_len(req), "\r\nConnection: close\r\nUser-Agent: Zeos/1.0\r\n\r\n");

    int sent = tls_send(conn, req, str_len(req));
    if (sent < 0) {
        tls_close(conn);
        return -1;
    }

    int total = 0;
    while (total < resp_max - 1) {
        int n = tls_recv(conn, resp_buf + total, resp_max - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    resp_buf[total] = 0;

    tls_close(conn);

    int status = -1;
    if (total > 12 && resp_buf[0] == 'H') {
        status = (resp_buf[9] - '0') * 100 +
                 (resp_buf[10] - '0') * 10 +
                 (resp_buf[11] - '0');
    }

    if (body_len) {
        *body_len = 0;
        for (int i = 0; i < total - 3; i++) {
            if (resp_buf[i] == '\r' && resp_buf[i+1] == '\n' &&
                resp_buf[i+2] == '\r' && resp_buf[i+3] == '\n') {
                *body_len = total - (i + 4);
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
