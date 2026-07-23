/*
 * Zeos — Spring Animation Engine
 *
 * Physics-based spring solver. Each tick integrates force, damping,
 * and acceleration for every active animation. When a spring settles
 * (position close to target AND velocity near zero), it snaps and
 * deactivates.
 *
 * Retarget is the key operation: change the destination of a running
 * spring without resetting velocity. This gives fluid, interruptible
 * motion — the hallmark of good UI physics.
 *
 * Bare-metal: no stdlib, no math.h. We inline what we need.
 */

#include "anim.h"
#include "theme.h"
#include "access.h"   /* reduced_motion / anim_speed accessibility consumers */

/* No math.h in bare-metal — inline fabs */
static inline float fabsf_z(float x) { return x < 0 ? -x : x; }

/* Animation pool */
static spring_anim_t anims[MAX_ANIMS];
static int anim_count;  /* active count cache */

void anim_init(void)
{
    anim_count = 0;
    for (int i = 0; i < MAX_ANIMS; i++)
        anims[i].active = 0;
}

int anim_spring(float from, float to, float stiffness, float damping,
                void (*on_update)(int, float, void*), void *ctx)
{
    /* Find a free slot — linear scan */
    for (int i = 0; i < MAX_ANIMS; i++) {
        if (!anims[i].active) {
            spring_anim_t *a = &anims[i];
            a->position  = from;
            a->velocity  = 0.0f;
            a->target    = to;
            a->stiffness = stiffness;
            a->damping   = damping;
            a->mass      = SPRING_MASS;
            a->active    = 1;
            a->start_tsc = 0;  /* Caller can set via timer_read_tsc() */
            a->on_update = on_update;
            a->ctx       = ctx;
            anim_count++;
            return i;
        }
    }
    return -1;  /* Pool full */
}

int anim_spring_default(float from, float to,
                        void (*on_update)(int, float, void*), void *ctx)
{
    return anim_spring(from, to, SPRING_STIFFNESS, SPRING_DAMPING,
                       on_update, ctx);
}

void anim_retarget(int anim_id, float new_target)
{
    if (anim_id < 0 || anim_id >= MAX_ANIMS)
        return;

    spring_anim_t *a = &anims[anim_id];
    if (!a->active)
        return;

    /* Change target, preserve velocity — smooth interruption */
    a->target = new_target;
}

void anim_cancel(int anim_id)
{
    if (anim_id < 0 || anim_id >= MAX_ANIMS)
        return;

    if (anims[anim_id].active) {
        anims[anim_id].active = 0;
        anim_count--;
    }
}

void anim_tick(float dt)
{
    /* Clamp dt to avoid explosion on long stalls */
    if (dt > 0.1f)
        dt = 0.1f;
    if (dt <= 0.0f)
        return;

    /* ── Accessibility consumers (M.1 reduced-motion, M.2 anim-speed) ──
     * reduced-motion OR 0x speed => INSTANT: snap every active spring to its
     * target this tick (no motion). Otherwise treat anim_speed as a duration
     * multiplier (0.5x = twice as fast, 2.0x = half speed) by scaling dt. */
    {
        access_config_t *acc = access_get();
        if (acc->reduced_motion || acc->anim_speed <= 0.01f) {
            for (int i = 0; i < MAX_ANIMS; i++) {
                spring_anim_t *a = &anims[i];
                if (!a->active) continue;
                a->position = a->target;
                a->velocity = 0.0f;
                a->active   = 0;
                anim_count--;
                if (a->on_update) a->on_update(i, a->position, a->ctx);
            }
            return;
        }
        dt /= acc->anim_speed;   /* 0.5 => faster, 2.0 => slower */
    }

    for (int i = 0; i < MAX_ANIMS; i++) {
        spring_anim_t *a = &anims[i];
        if (!a->active)
            continue;

        /*
         * Spring physics:
         *   F_spring  = (target - position) * stiffness
         *   F_damping = velocity * damping
         *   acceleration = (F_spring - F_damping) / mass
         *
         * Semi-implicit Euler integration.
         */
        float force = (a->target - a->position) * a->stiffness;
        float damping_force = a->velocity * a->damping;
        float acceleration = (force - damping_force) / a->mass;

        a->velocity += acceleration * dt;
        a->position += a->velocity * dt;

        /*
         * Settle check: if both displacement and velocity are
         * below threshold, snap to target and deactivate.
         */
        float dist = fabsf_z(a->position - a->target);
        float speed = fabsf_z(a->velocity);

        if (dist < SPRING_SETTLE_THRESHOLD && speed < SPRING_SETTLE_THRESHOLD) {
            a->position = a->target;
            a->velocity = 0.0f;
            a->active = 0;
            anim_count--;

            /* Final callback at exact target */
            if (a->on_update)
                a->on_update(i, a->position, a->ctx);
            continue;
        }

        /* Per-frame callback */
        if (a->on_update)
            a->on_update(i, a->position, a->ctx);
    }
}

float anim_position(int anim_id)
{
    if (anim_id < 0 || anim_id >= MAX_ANIMS)
        return 0.0f;
    return anims[anim_id].position;
}

int anim_is_active(int anim_id)
{
    if (anim_id < 0 || anim_id >= MAX_ANIMS)
        return 0;
    return anims[anim_id].active;
}

int anim_active_count(void)
{
    return anim_count;
}
