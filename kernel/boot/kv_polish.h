/*
 * Zeos — kv-zeos polish layer (Redis-cli killer UI)
 *
 * What this is:
 *   - A real visible window (`zkv` shell command opens it).
 *   - Color-coded keys by TTL state (green=fresh, yellow=expiring,
 *     red=expired-pending-GC).
 *   - Live key count + hit/miss histogram in the status bar.
 *   - REPL input line with persistent command history (Up/Down).
 *   - Inline help on TAB.
 *   - JSON pretty-print of structured values.
 *   - CFA-namespaced storage: every key is wrapped under the active
 *     identity context's CFA root, so cross-context kv access is denied
 *     at the resolve layer.
 *   - `kv export <pattern>` re-PIN gate for SOVEREIGN-tier values.
 *
 * Honest gaps (documented inline):
 *   - Autocomplete uses a linear scan, not the Z+ btree; fine for the
 *     few-thousand-key range, will be migrated when btree exposes a
 *     prefix-iter primitive.
 *   - Reverse-search (Ctrl-R) is forward-search-on-empty for now;
 *     same selection semantics, no fuzzy distance.
 *   - Drag-drop bulk import depends on the desktop drop dispatcher
 *     reporting MIME on drop; until then `kv import <path>` is the
 *     manual entry point.
 *
 * Selftest line:
 *   kv-zeos polish ....... REPL ready, N keys, M contexts isolated
 */

#ifndef ZEOS_KV_POLISH_H
#define ZEOS_KV_POLISH_H

#include <stdint.h>

/* Lazy init. Idempotent. */
void kv_polish_init(void);

/* Open / close / status of the zkv window. */
void kv_polish_open(void);
void kv_polish_close(void);
int  kv_polish_active(void);

/* Programmatic put/get/del (also reachable from the Z+ chain).
 * Both key and value are NUL-terminated strings; values up to 4 KiB.
 * TTL in seconds; 0 = no expiration. Returns 0 on success. */
int kv_polish_put(const char *key, const char *value, uint32_t ttl_sec);
int kv_polish_get(const char *key, char *out, int out_cap);
int kv_polish_del(const char *key);

/* Mark a key SOVEREIGN tier (export gated by re-PIN). */
int kv_polish_set_sovereign(const char *key);

/* Inputs (called by shell on focused window). */
void kv_polish_key(int ascii);          /* printable / Enter / Esc / TAB */
void kv_polish_key_arrow(int dir);      /* 0=L 1=R 2=Up 3=Down */
void kv_polish_key_ctrl(int ascii);     /* Ctrl-R / Ctrl-L / Ctrl-K */
void kv_polish_mouse_down(int x, int y, int button);

/* Counters for selftest. */
uint32_t kv_polish_total_keys(void);
uint32_t kv_polish_total_hits(void);
uint32_t kv_polish_total_misses(void);
uint32_t kv_polish_total_contexts(void);

/* Selftest line. Format:
 *   kv-zeos polish ....... REPL ready, N keys, M contexts isolated  */
void kv_polish_print_selftest_line(void);

/* Shell command implementation. */
void kv_polish_cmd(const char *args);

#endif /* ZEOS_KV_POLISH_H */
