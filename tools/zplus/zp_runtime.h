/* zp_runtime.h — thin shim between generated Z+ code and the Zeos kernel.
 *
 * When compiled against the Zeos kernel, include chain.h, vault.h, tap.h.
 * When compiled on Linux for testing, provide stub implementations.
 *
 * Generated code only ever calls:
 *   chain_create(), chain_add_node(), chain_node_t
 *   zp_tap_log(), zp_vault_put(), zp_vault_get()
 *   zp_time_now_ms(), zp_http_listen()
 *   zp_str_split(), zp_array_len()
 *   btree_t, btree_insert(), btree_get(), btree_delete()
 */
#pragma once
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stddef.h>

#ifdef ZEOS_KERNEL
/* ── kernel build: real headers ─────────────────────────────────── */
#include "chain.h"
#include "vault.h"

static inline void zp_tap_log(const char *name, void *sig) {
    (void)name; (void)sig; /* tap.log -> kprint in kernel */
}
static inline void zp_vault_put(const char *key, void *val) {
    vault_put(key, val, 0);
}
static inline void *zp_vault_get(const char *key, void *query) {
    (void)query; return vault_get(key);
}
static inline uint64_t zp_time_now_ms(void) {
    extern uint64_t tod_now_ms(void);
    return tod_now_ms();
}

#else
/* ── host build: stubs for test/dev on Linux ────────────────────── */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* minimal chain_node_t / chain_t stub so generated code compiles */
typedef struct chain_node {
    char  name[64];
    char  input_type[32];
    char  output_type[32];
    void (*resolve)(struct chain_node *self, void *input, void *output);
    void *state;
} chain_node_t;

static inline int chain_create(const char *name, int parent, int tier) {
    static int _next = 0;
    (void)parent; (void)tier;
    printf("[chain_create] %s -> id=%d\n", name, _next);
    return _next++;
}

static inline int chain_add_node(int chain_id, const char *name,
                                  const char *sig_in, const char *sig_out,
                                  void (*resolve_fn)(chain_node_t*, void*, void*)) {
    printf("[chain_add_node] chain=%d  node=%-20s  %s -> %s  fn=%s\n",
           chain_id, name, sig_in, sig_out,
           resolve_fn ? "(fn)" : "NULL");
    return 0;
}

static inline void zp_tap_log(const char *name, void *sig) {
    printf("[tap.log] %s: %p\n", name, sig);
}
static inline void zp_vault_put(const char *key, void *val) {
    printf("[vault.put] %s = %p\n", key, val);
}
static inline void *zp_vault_get(const char *key, void *query) {
    (void)query; printf("[vault.get] %s\n", key); return NULL;
}
static inline uint64_t zp_time_now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}
static inline void zp_http_listen(void *state, void *port) {
    (void)state; printf("[http.listen] port=%p\n", port);
}
static inline void *zp_str_split(const char *s, const char *sep) {
    (void)sep; printf("[str.split] \"%s\"\n", s ? s : ""); return NULL;
}
static inline long zp_array_len(void *arr) {
    (void)arr; return 0;
}

/* ── signal field access (used by gate conditions pre-type-propagation) ── */
/* Return the named field as a C string.  Stub returns empty string.
 * When type propagation lands, replace with typed struct accessor. */
static inline const char *zp_field_str(void *sig, const char *field) {
    (void)sig; (void)field; return "";
}
/* Return the named field as a double.  Stub returns 0. */
static inline double zp_field_dbl(void *sig, const char *field) {
    (void)sig; (void)field; return 0.0;
}
/* Return the whole signal as a double (for untyped numeric signals). */
static inline double zp_signal_dbl(void *sig) {
    if (!sig) return 0.0;
    return (double)(*(long*)sig);
}

/* minimal btree stub */
typedef struct { void *root; } btree_t;
static inline void btree_insert(btree_t *t, void *v) { (void)t; (void)v; }
static inline void *btree_get(btree_t *t, void *k)   { (void)t; (void)k; return NULL; }
static inline void btree_delete(btree_t *t, void *k)  { (void)t; (void)k; }

/* masq tier constants (mirror chain.h) */
#define MASQ_SOVEREIGN  0
#define MASQ_INTERNAL   1
#define MASQ_REFERENCE  2

#endif /* ZEOS_KERNEL */
