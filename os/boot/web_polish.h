/*
 * Zeos — web-zeos polish layer (nginx admin UI killer)
 *
 * What this is:
 *   - Real visible window (`webview` shell command).
 *   - Live request log scrolling at the bottom, color-coded by status
 *     (2xx green / 3xx amber / 4xx orange / 5xx red).
 *   - Per-route req/s sparkline and a routes editor pane.
 *   - TLS cert pane: Subject + SAN summary + days-to-expiry.
 *   - Context menu / right-click affordances.
 *
 * Security:
 *   - TLS keys are CFA SOVEREIGN handles (allocator wraps incoming PEM
 *     in cfa_wrap with MASQ_SOVEREIGN before stashing).
 *   - Per-route rate limit knob persisted via the firewall conntrack.
 *   - CSP / HSTS / X-Content-Type-Options headers default-on for static
 *     responses (web_polish_inject_default_headers wired into the
 *     std.http.respond path).
 *
 * Honest gaps:
 *   - The TCP server side (commit dff56f5) is real; cert hot-reload is
 *     wired through net_tls but the drag-drop dispatcher reporting MIME
 *     on the window isn't yet, so cert install today is via shell:
 *     `web tls /path/to/cert.pem /path/to/key.pem`.
 *   - Routes are currently a flat in-memory list (16 max); persistence
 *     to vault is wired but reorder-by-drag emits an event that the
 *     vault subscriber consumes (it works; the visual drag handle is
 *     the gap).
 *
 * Selftest line:
 *   web-zeos polish ...... admin UI ready, N routes, M certs
 */

#ifndef ZEOS_WEB_POLISH_H
#define ZEOS_WEB_POLISH_H

#include <stdint.h>

void web_polish_init(void);

void web_polish_open(void);
void web_polish_close(void);
int  web_polish_active(void);

/* Add a route. method = "GET" | "POST" | "*"; path is exact-match
 * (TODO: globbing via std.regex.match). Returns 0 on success. */
int  web_polish_route_add(const char *method, const char *path, const char *handler);
int  web_polish_route_count(void);

/* Record a request for the live log + per-route sparkline. */
void web_polish_log_request(const char *method, const char *path,
                            int status, uint32_t latency_us);

/* Install a cert (PEM strings). Wraps the key buffer in a SOVEREIGN
 * CFA handle before storage; the raw key is zeroed after wrap. */
int  web_polish_install_cert(const char *cert_pem, const char *key_pem,
                             int cert_len, int key_len);
int  web_polish_cert_count(void);

void web_polish_print_selftest_line(void);
void web_polish_cmd(const char *args);

#endif /* ZEOS_WEB_POLISH_H */
