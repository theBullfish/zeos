/*
 * Zeos -- Codex Fractal Addressing handle layer (kernel-side).
 *
 * Wraps security-relevant memory (TLS keys, VAULT blobs) so access goes
 * through MasQ tier checks rather than raw pointers. Hardware DMA still
 * requires flat pointers; cfa_resolve() is the boundary.
 *
 * Rust lib at ~/cfa-lib (TPM-sealed, patent filed 2026-03-15) is the
 * userspace mirror -- kernel side keeps a minimal compatible API.
 */

#ifndef ZEOS_CFA_HANDLE_H
#define ZEOS_CFA_HANDLE_H

#include <stdint.h>
#include <stddef.h>
#include "chain.h"

typedef uint64_t cfa_handle_t;

/* Categories used purely for selftest accounting. Wrapping callers can
 * tag the handle so the selftest can render a per-subsystem breakdown
 * (TLS conf, TLS sessions, VAULT blob, VAULT keys, MSI-X table, GPU
 * cmd buf). Tag value is opaque to the security checks -- those are
 * driven by tier alone. */
typedef enum {
    CFA_CAT_OTHER = 0,
    CFA_CAT_TLS_CONF,
    CFA_CAT_TLS_SESSION,
    CFA_CAT_VAULT_BLOB,
    CFA_CAT_VAULT_KEY,
    CFA_CAT_MSIX_TABLE,
    CFA_CAT_GPU_CMD_BUF,
    CFA_CAT__COUNT
} cfa_category_t;

/* Wrap a flat pointer in a CFA handle. The (addr, tier) pair governs
 * MasQ-tier access checks at resolve time. Returns 0 on failure. */
cfa_handle_t cfa_wrap(void *ptr, size_t len, cfa_addr_t addr, masq_tier_t tier);

/* Tagged variant -- same as cfa_wrap but records a category for
 * selftest accounting. Use the untagged variant for ad-hoc wraps. */
cfa_handle_t cfa_wrap_cat(void *ptr, size_t len, cfa_addr_t addr,
                          masq_tier_t tier, cfa_category_t cat);

/* Live count for a specific category (selftest reporting). */
int cfa_handle_count_cat(cfa_category_t cat);

/* Resolve a handle to its raw pointer. Returns NULL if:
 *   - handle is 0 / invalid / generation-stale
 *   - the current observer chain (per cfa_set_observer) cannot perceive
 *     the wrapped tier per MasQ rules
 *
 * If no observer is set (cfa_set_observer(-1) or never called), resolve
 * succeeds. Hardware DMA backends typically run with CHAIN_CPU as
 * observer (sees everything internal+ref) so they continue working.
 */
void *cfa_resolve(cfa_handle_t h);

/* Release a handle. Bumps the slot generation so any cached copies of
 * the old handle become permanently invalid. Idempotent. */
void  cfa_release(cfa_handle_t h);

/* Number of currently-live handles (for selftest). */
int   cfa_handle_count(void);

/* Set / clear the calling chain context. chain_resolve() sets this
 * before invoking each node so cfa_resolve can apply MasQ rules.
 * Pass -1 to clear (no observer == permissive). */
void  cfa_set_observer(int chain_id);

/* Read the current observer (debugging). */
int   cfa_get_observer(void);

#endif /* ZEOS_CFA_HANDLE_H */
