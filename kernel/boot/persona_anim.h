/*
 * Zeos — Persona Transition Animation
 *
 * Spring-animated color crossfade between personas. When the user
 * types "zeros", "derez", or "raise", the accent color doesn't snap —
 * it crossfades through a SPRING_SMOOTH (100/16) spring over ~300ms.
 *
 * During the transition, all rendering that uses the persona accent
 * should call persona_current_accent() instead of the static color.
 * When the spring settles, final persona settings are applied
 * (cursor colorway, MasQ filter, panel persona) and the transition
 * ends.
 */

#ifndef ZEOS_PERSONA_ANIM_H
#define ZEOS_PERSONA_ANIM_H

#include <stdint.h>

/* Start a persona transition with spring-animated color crossfade */
void persona_transition(int from_persona, int to_persona);

/* Get the current interpolated accent color (during transition) */
uint32_t persona_current_accent(void);
uint32_t persona_current_accent_dim(void);

/* Is a transition in progress? */
int persona_transitioning(void);

/* Tick (called per frame) */
void persona_anim_tick(void);

#endif /* ZEOS_PERSONA_ANIM_H */
