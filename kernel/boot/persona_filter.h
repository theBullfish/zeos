/*
 * Zeos -- Persona MasQ Filter
 *
 * The progressive curtain. Maps the three personas (Zeros, DereZ, Zeos)
 * to MasQ perception tiers. A Zeros kid sees REFERENCE chains only.
 * A DereZ dev sees REFERENCE + INTERNAL. Full Zeos sees everything.
 *
 * The system isn't hiding anything -- those chains literally don't exist
 * from the persona's perception. That's MasQ.
 */

#ifndef ZEOS_PERSONA_FILTER_H
#define ZEOS_PERSONA_FILTER_H

#include "chain.h"
#include "persona.h"

/* Set the active persona (affects what chains are visible system-wide) */
void persona_set_active(int persona);  /* 0=zeros, 1=derez, 2=full */

/* Get the currently-active persona (returns enum persona). */
int persona_get_active(void);

/* Get the MasQ tier that the current persona can perceive up to */
masq_tier_t persona_max_tier(void);

/* Check if the current persona can see a given chain */
int persona_can_see(int chain_id);

/* Filter: how many chains are visible to current persona */
int persona_visible_chain_count(void);

#endif /* ZEOS_PERSONA_FILTER_H */
