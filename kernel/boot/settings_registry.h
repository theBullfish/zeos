/*
 * Zeos -- Unified settings registry.
 *
 * Every config knob in the system registers a getter/setter pair
 * through this surface. The shell `settings` command is the single
 * source of truth for "what can the user change?" Each module
 * continues to own its own VAULT persistence — we just expose the
 * surface here.
 *
 * Settings are typed (kind) so the shell can format/parse them
 * consistently. Values flow through the registry as null-terminated
 * strings; the getter/setter pair owned by the source module is
 * responsible for any conversion.
 */

#ifndef ZEOS_SETTINGS_REGISTRY_H
#define ZEOS_SETTINGS_REGISTRY_H

#include <stdint.h>

#define SETTINGS_REGISTRY_MAX     64
#define SETTINGS_VALUE_MAX        64
#define SETTINGS_NAME_MAX         48

typedef enum {
    SK_STRING   = 0,   /* free-form text */
    SK_INT      = 1,   /* signed integer */
    SK_INT_PCT  = 2,   /* 0..100 percent */
    SK_INT_SECS = 3,   /* seconds */
    SK_BOOL     = 4,   /* 0 / 1 */
    SK_ENUM     = 5,   /* one of fixed labels (held in `enum_labels`) */
} settings_kind_t;

#define SETTINGS_FLAG_READONLY   0x01u
#define SETTINGS_FLAG_WRITEONLY  0x02u    /* getter returns "***" */
#define SETTINGS_FLAG_STUB       0x04u    /* source module not yet wired */

/* Getter writes the current value into `out` (null-terminated, capped
 * at SETTINGS_VALUE_MAX). Returns 0 on success, -1 if not available. */
typedef int (*settings_getter_t)(char *out, int out_len);

/* Setter parses `value` and applies it. Returns 0 on success, -1 on
 * parse / range / not-supported (ENOTSUPP). */
typedef int (*settings_setter_t)(const char *value);

typedef struct {
    const char        *name;        /* dotted, e.g. "audio.volume" */
    settings_kind_t    kind;
    uint32_t           flags;
    settings_getter_t  getter;
    settings_setter_t  setter;
    const char        *enum_labels; /* "a|b|c" for SK_ENUM, else NULL */
    const char        *desc;        /* one-line human description */
} settings_entry_t;

/* Register a single setting. Returns 0 on success, -1 if table full or
 * a duplicate name exists. The `entry` pointer must be statically
 * allocated; the registry stores it by reference. */
int  settings_register(const settings_entry_t *entry);

/* Look up by name. Returns NULL if not found. */
const settings_entry_t *settings_find(const char *name);

/* Read current value into `out`. Returns 0 on success, -1 on miss
 * or getter error. Write-only settings return "***". */
int  settings_get(const char *name, char *out, int out_len);

/* Apply a new value. Returns 0 on success, -1 on miss / readonly /
 * setter error. */
int  settings_set(const char *name, const char *value);

/* Walk every registered setting in registration order. The callback
 * receives the entry plus an opaque user pointer; returning non-zero
 * stops the walk. */
typedef int (*settings_walk_cb)(const settings_entry_t *e, void *user);
int  settings_enumerate(settings_walk_cb cb, void *user);

/* Total count of currently-registered settings. */
int  settings_count(void);

/* How many of those have flags == 0 (i.e. fully wired, not stubs). */
int  settings_live_count(void);

/* Bulk-register every system setting we know about. Idempotent;
 * called once from main.c after all source modules are up. */
void settings_register_all(void);

/* Selftest line printer — "Settings .............. N keys ..." */
void settings_print_selftest_line(void);

/* Register CHAIN_SETTINGS (parent CHAIN_CPU, MASQ_INTERNAL) with the
 * 4-node pipeline:
 *   setting_change_request -> validate -> apply -> record
 *
 * After registration, settings_set() routes through this chain. The
 * chain's vault_version bumps once per accepted change so MasQ records
 * every {key, old, new} mutation as provenance. Idempotent. Returns
 * the new chain id, or -1 on failure. */
int settings_chain_register(int parent_chain_id);

/* Lifetime count of accepted setting changes that flowed through the
 * chain. Selftest reads this to prove the pipeline is alive. */
uint32_t settings_chain_apply_total(void);

/* Kind label, e.g. "INT_PCT". Stable strings, no allocation. */
const char *settings_kind_label(settings_kind_t k);

#endif /* ZEOS_SETTINGS_REGISTRY_H */
