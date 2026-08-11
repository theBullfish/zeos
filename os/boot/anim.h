/*
 * Zeos — Spring Animation System
 *
 * Physics-based spring animations for UI motion. No keyframes,
 * no easing curves. Springs settle naturally — interruption is
 * free (retarget preserves velocity, no jarring snaps).
 *
 * All animations are driven by anim_tick() each frame.
 * Callbacks fire with the resolved position so the caller
 * can move whatever it needs (a rect, a color, an opacity).
 *
 * This is bare-metal. No stdlib, no math.h.
 */

#ifndef ZEOS_ANIM_H
#define ZEOS_ANIM_H

#include <stdint.h>

/* Maximum concurrent animations */
#define MAX_ANIMS 64

typedef struct {
    /* Spring state */
    float position;
    float velocity;
    float target;

    /* Spring parameters */
    float stiffness;
    float damping;
    float mass;

    /* Metadata */
    uint8_t active;
    uint64_t start_tsc;     /* TSC when started */

    /* Callback: what to do each frame with the resolved position */
    void (*on_update)(int anim_id, float position, void *ctx);
    void *ctx;
} spring_anim_t;

/* Initialize the animation system */
void anim_init(void);

/* Create a new spring animation. Returns anim_id or -1 if full. */
int anim_spring(float from, float to, float stiffness, float damping,
                void (*on_update)(int, float, void*), void *ctx);

/* Create with default spring constants */
int anim_spring_default(float from, float to,
                        void (*on_update)(int, float, void*), void *ctx);

/* Retarget a running animation (velocity preserved) */
void anim_retarget(int anim_id, float new_target);

/* Cancel an animation */
void anim_cancel(int anim_id);

/* Tick all active animations. Call this every frame.
   dt = seconds since last frame (from TSC). */
void anim_tick(float dt);

/* Get current position of an animation */
float anim_position(int anim_id);

/* Check if animation is still active */
int anim_is_active(int anim_id);

/* Count of active animations */
int anim_active_count(void);


/* L.6: spring scroll physics — momentum + edge rubber-band. */
typedef struct { float pos; float vel; float max; } scroll_phys_t;
void scroll_phys_init(scroll_phys_t *s, float max_offset);
void scroll_phys_flick(scroll_phys_t *s, float velocity);
void scroll_phys_tick(scroll_phys_t *s, float dt);
int  scroll_phys_active(const scroll_phys_t *s);

#endif /* ZEOS_ANIM_H */
