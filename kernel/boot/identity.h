/*
 * Zeos -- CFA identity contexts.
 *
 * Each context is a CFA root with its own MasQ tier policy, chain
 * visibility set, disk encryption key, and namespace. Login = switching
 * the active root. The Zeos primitive replaces UNIX-style users / UIDs
 * / sudo entirely.
 *
 * Cross-context perception is limited to REFERENCE-tier chains;
 * INTERNAL and SOVEREIGN are scoped to their own context.
 */

#ifndef ZEOS_IDENTITY_H
#define ZEOS_IDENTITY_H

#include <stdint.h>
#include "chain.h"
#include "cfa_handle.h"

#define IDENTITY_MAX_CONTEXTS     16
#define IDENTITY_NAME_MAX         32
#define IDENTITY_OWNER_CHAINS_MAX 32
#define IDENTITY_PIN_MIN_LEN      4
#define IDENTITY_PIN_MAX_LEN      16

/* A single identity context. The CFA root is the address every chain
 * created while this context is active derives from. Other contexts
 * cannot derive a child of a different context's root because the root
 * index is unique per context (derived from the slot id) and the depth
 * vector is stamped at creation. */
typedef struct identity_context {
    int            id;                 /* 1-based slot; 0 = unused */
    char           name[IDENTITY_NAME_MAX];

    /* CFA root: addr.depth=1, segments[0]=IDENTITY_ROOT_BASE+id. All
     * chains created while this context is active will derive from
     * `cfa_root_addr` via cfa_derive(). */
    cfa_addr_t     cfa_root_addr;

    /* Default tier for chains created under this context. Most contexts
     * use INTERNAL; a "guest" context could use REFERENCE. */
    masq_tier_t    masq_tier_default;

    /* PIN handle (CFA-wrapped at SOVEREIGN). The backing buffer lives
     * inside identity.c. cfa_resolve gates access via the SOVEREIGN
     * tier check. */
    cfa_handle_t   pin_handle;

    /* Chain ids owned by this context (created while it was active). */
    int            owner_chains[IDENTITY_OWNER_CHAINS_MAX];
    int            owner_chains_count;

    uint64_t       created_tsc;
    uint64_t       last_active_tsc;
} identity_context_t;

/* Module init. Loads contexts from VAULT, migrates legacy single-PIN
 * boot (the /lock/pin + /crypto/salt pair) into context id=1 "owner"
 * if no contexts exist yet. Idempotent. */
void identity_init(void);

/* Create a new context. Allocates a slot, derives a fresh CFA root,
 * stores the PIN at SOVEREIGN tier, persists the context record.
 * Returns the new context id (>=1) on success, -1 on failure
 * (duplicate name, slot exhaustion, malformed PIN, ...). */
int  identity_context_create(const char *name, const char *pin);

/* Switch the active context. Verifies the supplied PIN against the
 * stored PIN for `id` (constant-time). On success: re-derives the
 * disk encryption key from this context's PIN + per-context salt,
 * stamps last_active_tsc, journals the switch, returns 0.
 * Returns -1 on auth failure or invalid id. */
int  identity_context_switch(int id, const char *pin);

/* Returns the active context id (>=1), or 0 if none active yet. */
int  identity_context_active(void);

/* Returns the active context's CFA root address (or a zeroed addr if
 * no active context). chain_create / cfa_resolve use this to derive
 * fresh chain CFA roots and to compare cross-context perception. */
cfa_addr_t identity_active_cfa_root(void);

/* Walk all contexts. callback(ctx, user) is invoked once per live
 * slot. Returns the number of contexts visited. */
int  identity_context_list(void (*cb)(const identity_context_t *ctx, void *user),
                           void *user);

/* Look up by name. Returns the context id or -1. */
int  identity_context_by_name(const char *name);

/* Delete a context. Securely zeroes its PIN buffer, removes the slot
 * from VAULT, destroys chains in owner_chains[] (best-effort), and
 * wipes its salt + PIN keys. The currently-active context cannot be
 * deleted unless it is the only context (a guard against locking
 * yourself out by accident); use a different active context to delete
 * one by id. Returns 0 on success, -1 on failure. */
int  identity_context_delete(int id);

/* Cross-context perception gate. Returns 1 if observer_root and
 * target_root share the same context root segment OR target_tier ==
 * MASQ_REFERENCE. Used by chain_can_perceive to extend the tier check
 * with a CFA-root scope check. */
int  identity_roots_same_context(const cfa_addr_t *a, const cfa_addr_t *b);

/* Used by chain.c when a new chain is created. Records the chain id
 * under the active context's owner_chains[] for later cleanup on
 * context delete. No-op if no context is active yet (which happens
 * during very-early boot, before identity_init). */
void identity_register_chain(int chain_id);

/* The CFA segment range identity contexts occupy. Exposed so
 * chain.c can recognise the root and chain.c's cross-context check
 * can be cheap. */
#define IDENTITY_ROOT_BASE  0xC0DE0000u  /* "CODE" -- segment[0] of every
                                             context CFA root */

/* Selftest line emitted from main.c after init. */
void identity_print_selftest_line(void);

/* ── Vault namespacing helpers ────────────────────────────────────
 *
 * Prepends "/ctx/<id>" to the given path so the same logical key in
 * two different contexts maps to two different VAULT inodes. Returns
 * the byte length written (excluding the NUL), or -1 on overflow.
 * If no context is active or `path` already begins with "/ctx/" the
 * input is copied verbatim (legacy paths and global tools).
 */
int  identity_namespace_path(const char *path, char *out, int out_cap);

/* Shell command implementations (registered in shell.c). */
void cmd_whoami(const char *args);
void cmd_contexts(const char *args);
void cmd_switch_context(const char *args);
void cmd_new_context(const char *args);
void cmd_delete_context(const char *args);

#endif /* ZEOS_IDENTITY_H */
