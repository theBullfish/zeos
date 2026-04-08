/*
 * Zeos -- Persona MasQ Filter (implementation)
 *
 * Static active persona. No malloc. Bare-metal.
 *
 * Perception rules by persona:
 *   Zeros  -> MASQ_REFERENCE only
 *   DereZ  -> MASQ_REFERENCE + MASQ_INTERNAL
 *   Full   -> MASQ_REFERENCE + MASQ_INTERNAL + MASQ_SOVEREIGN
 *
 * MasQ tier ordering (from chain.h):
 *   MASQ_SOVEREIGN = 0   (most restricted)
 *   MASQ_INTERNAL  = 1
 *   MASQ_REFERENCE = 2   (most public)
 *
 * A persona can see a chain if chain->tier >= persona_max_tier().
 * Higher tier number = more public = more visible.
 */

#include "persona_filter.h"
#include "kprint.h"

/* ── Static state ───────────────────────────────────────────────── */

static int active_persona = PERSONA_FULL;

/* ── API ────────────────────────────────────────────────────────── */

void persona_set_active(int persona)
{
    if (persona < PERSONA_ZEROS || persona > PERSONA_FULL)
        persona = PERSONA_FULL;

    active_persona = persona;

    kputs("[persona] active=");
    switch (persona) {
    case PERSONA_ZEROS: kputs("Zeros (REFERENCE only)");  break;
    case PERSONA_DEREZ: kputs("DereZ (REFERENCE+INTERNAL)"); break;
    case PERSONA_FULL:  kputs("Zeos  (all tiers)");       break;
    }
    kputc('\n');
}

masq_tier_t persona_max_tier(void)
{
    /*
     * Returns the highest-secrecy tier this persona can perceive.
     * Since MASQ_SOVEREIGN=0 < MASQ_INTERNAL=1 < MASQ_REFERENCE=2,
     * a persona can see chains where chain->tier >= max_tier.
     *
     * Zeros:  can only see REFERENCE (tier >= 2)
     * DereZ:  can see INTERNAL + REFERENCE (tier >= 1)
     * Full:   can see everything (tier >= 0)
     */
    switch (active_persona) {
    case PERSONA_ZEROS: return MASQ_REFERENCE;
    case PERSONA_DEREZ: return MASQ_INTERNAL;
    case PERSONA_FULL:  return MASQ_SOVEREIGN;
    }
    return MASQ_SOVEREIGN;  /* default: full visibility */
}

int persona_can_see(int chain_id)
{
    chain_t *c = chain_get(chain_id);
    if (!c) return 0;

    /*
     * Chain is visible if its tier is at or above (numerically >=)
     * the persona's max tier threshold.
     *
     * Example: Zeros max_tier = MASQ_REFERENCE (2)
     *   SOVEREIGN chain (0) -> 0 >= 2? No.  Hidden.
     *   INTERNAL  chain (1) -> 1 >= 2? No.  Hidden.
     *   REFERENCE chain (2) -> 2 >= 2? Yes. Visible.
     */
    return (int)c->tier >= (int)persona_max_tier();
}

int persona_visible_chain_count(void)
{
    int visible = 0;
    int i;

    for (i = 0; i < MAX_CHAINS; i++) {
        chain_t *c = chain_get(i);
        if (!c) continue;
        if (persona_can_see(i))
            visible++;
    }

    return visible;
}
