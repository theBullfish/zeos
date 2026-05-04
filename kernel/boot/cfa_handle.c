/*
 * Codex Fractal Addressing -- kernel-side handle layer.
 * Wraps security-relevant memory (TLS keys, VAULT blobs) so access
 * goes through MasQ tier checks rather than raw pointers. Hardware
 * DMA still requires flat pointers; cfa_resolve() is the boundary.
 * Rust lib at ~/cfa-lib (TPM-sealed, patent filed 2026-03-15) is the
 * userspace mirror -- kernel side keeps a minimal compatible API.
 */

#include "cfa_handle.h"
#include "chain.h"
#include "kprint.h"

#define CFA_SLOT_COUNT  256

typedef struct {
    void        *ptr;
    size_t       len;
    cfa_addr_t   addr;
    masq_tier_t  tier;
    uint32_t     generation;   /* bumps on release */
    uint8_t      in_use;
} cfa_slot_t;

static cfa_slot_t s_slots[CFA_SLOT_COUNT];
static int        s_live_count;

/*
 * Observer chain context. In a bare-metal single-threaded kernel this
 * is effectively thread-local: chain_resolve sets it before invoking a
 * node, then clears it. Any cfa_resolve() call inside a node body sees
 * the current chain id; calls outside any chain context (boot init,
 * shell prompt, hardware DMA) get the permissive default of -1.
 */
static int s_observer = -1;

void cfa_set_observer(int chain_id)
{
    s_observer = chain_id;
}

int cfa_get_observer(void)
{
    return s_observer;
}

/* Same tier rule as chain_can_perceive() but against a stored tier
 * instead of a target chain. Sovereign sees all; Internal sees
 * Internal+Reference; Reference sees Reference only. */
static int tier_can_perceive(masq_tier_t observer_tier, masq_tier_t target_tier)
{
    switch (observer_tier) {
    case MASQ_SOVEREIGN: return 1;
    case MASQ_INTERNAL:  return (target_tier == MASQ_INTERNAL ||
                                  target_tier == MASQ_REFERENCE);
    case MASQ_REFERENCE: return (target_tier == MASQ_REFERENCE);
    }
    return 0;
}

cfa_handle_t cfa_wrap(void *ptr, size_t len, cfa_addr_t addr, masq_tier_t tier)
{
    if (!ptr) return 0;

    int i;
    for (i = 0; i < CFA_SLOT_COUNT; i++) {
        if (!s_slots[i].in_use) {
            s_slots[i].ptr        = ptr;
            s_slots[i].len        = len;
            s_slots[i].addr       = addr;
            s_slots[i].tier       = tier;
            s_slots[i].in_use     = 1;
            /* Generation starts at 1 so handle 0 is always invalid. */
            if (s_slots[i].generation == 0) s_slots[i].generation = 1;
            s_live_count++;
            return ((uint64_t)(uint32_t)i << 32) | (uint64_t)s_slots[i].generation;
        }
    }

    kputs("[cfa] ERROR: handle table full\n");
    return 0;
}

void *cfa_resolve(cfa_handle_t h)
{
    if (h == 0) return (void *)0;

    uint32_t slot_idx   = (uint32_t)(h >> 32);
    uint32_t generation = (uint32_t)(h & 0xFFFFFFFFu);

    if (slot_idx >= CFA_SLOT_COUNT) return (void *)0;
    cfa_slot_t *s = &s_slots[slot_idx];

    if (!s->in_use)                  return (void *)0;
    if (s->generation != generation) return (void *)0;  /* stale */

    /* MasQ enforcement. If no observer set, default permissive
     * (boot init, hardware DMA backends). Otherwise check that the
     * observer's tier can perceive the wrapped tier. */
    if (s_observer >= 0) {
        chain_t *obs = chain_get(s_observer);
        if (obs) {
            if (!tier_can_perceive(obs->tier, s->tier))
                return (void *)0;
        }
        /* If observer id was set but the chain has been destroyed,
         * treat as permissive rather than dropping data. */
    }

    return s->ptr;
}

void cfa_release(cfa_handle_t h)
{
    if (h == 0) return;

    uint32_t slot_idx   = (uint32_t)(h >> 32);
    uint32_t generation = (uint32_t)(h & 0xFFFFFFFFu);

    if (slot_idx >= CFA_SLOT_COUNT) return;
    cfa_slot_t *s = &s_slots[slot_idx];

    if (!s->in_use) return;
    if (s->generation != generation) return;

    s->in_use = 0;
    s->ptr    = (void *)0;
    s->len    = 0;
    /* Bump generation so any other copy of this handle becomes stale.
     * Skip 0 (reserved as invalid). */
    s->generation++;
    if (s->generation == 0) s->generation = 1;
    s_live_count--;
}

int cfa_handle_count(void)
{
    return s_live_count;
}
