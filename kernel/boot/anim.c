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

/* Max spring-integration sub-step (~1/240s). Frame dt is split into steps no
 * larger than this so a stiff spring stays numerically stable regardless of how
 * long the real frame took (see the sub-step comment in anim_tick). */
#define ANIM_SUBSTEP_MAX (1.0f / 240.0f)

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
         * Spring physics, semi-implicit Euler:
         *   F_spring  = (target - position) * stiffness
         *   F_damping = velocity * damping
         *   acceleration = (F_spring - F_damping) / mass
         *
         * SUB-STEPPED (fix, 2026-07-25): a single Euler step of the full frame
         * dt is numerically UNSTABLE for a stiff ("snappy") spring once dt gets
         * large -- stability needs roughly dt < 2/sqrt(stiffness/mass), which a
         * snappy spring violates at the ~100ms frame times seen under QEMU-TCG
         * (frame_dt is real TSC-elapsed, B.4). The result was a diverging
         * oscillation: maximize sprang a window's geometry to exploding
         * off-screen values so it "vanished" (C.5). Integrating in fixed small
         * sub-steps keeps every step well inside the stability region
         * regardless of frame_dt, and also hardens real hardware against frame
         * hitches. Max 0.1s / (1/240s) = 24 sub-steps -- cheap. */
        /* M.4: LOW_STIMULI softens spring stiffness (0.5x) for gentler motion. */
        float k = a->stiffness * access_spring_stiffness_scale();

        float remaining = dt;
        while (remaining > 0.0f) {
            float h = (remaining > ANIM_SUBSTEP_MAX) ? ANIM_SUBSTEP_MAX : remaining;
            float force = (a->target - a->position) * k;
            float damping_force = a->velocity * a->damping;
            float acceleration = (force - damping_force) / a->mass;
            a->velocity += acceleration * h;
            a->position += a->velocity * h;
            remaining -= h;
        }

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

#ifdef ZEOS_DIAG_L1
#include "kprint.h"
static void l1_cb(int id, float v, void *ctx) { (void)id; (void)v; (void)ctx; }
/* L.1 selftest: prove the spring engine's three claims.
 *  - semi-implicit Euler: a spring moves GRADUALLY (0<pos<target mid-flight) and
 *    CONVERGES to target then deactivates on settle.
 *  - 64 concurrent: MAX_ANIMS springs coexist; the 65th allocation fails (-1).
 *  - retarget-with-velocity: retarget mid-flight leaves velocity untouched and
 *    the spring then converges to the NEW target. */
void anim_l1_selftest(void)
{
    /* 1. Euler convergence + gradual motion */
    anim_init();
    int id = anim_spring(0.0f, 100.0f, SPRING_STIFFNESS, SPRING_DAMPING, l1_cb, 0);
    for (int i = 0; i < 3; i++) anim_tick(1.0f / 240.0f);
    float mid = anims[id].position;
    int gradual = (mid > 0.0f && mid < 100.0f);
    for (int i = 0; i < 4000; i++) anim_tick(1.0f / 240.0f);
    int settled   = (anims[id].active == 0);
    int converged = (fabsf_z(anims[id].position - 100.0f) < 0.5f);

    /* 2. 64 concurrent + pool-full */
    anim_init();
    int spawned = 0;
    for (int i = 0; i < MAX_ANIMS; i++)
        if (anim_spring(0.0f, 1.0f, SPRING_STIFFNESS, SPRING_DAMPING, l1_cb, 0) >= 0)
            spawned++;
    int full_count = (spawned == MAX_ANIMS) && (anim_active_count() == MAX_ANIMS);
    int overflow   = (anim_spring(0.0f, 1.0f, SPRING_STIFFNESS, SPRING_DAMPING, l1_cb, 0) == -1);

    /* 3. retarget preserves velocity */
    anim_init();
    int r = anim_spring(0.0f, 100.0f, SPRING_STIFFNESS, SPRING_DAMPING, l1_cb, 0);
    for (int i = 0; i < 5; i++) anim_tick(1.0f / 240.0f);
    float vel_before = anims[r].velocity;
    anim_retarget(r, 50.0f);
    float vel_after = anims[r].velocity;
    int vel_preserved = (vel_before > 0.0f) && (vel_after == vel_before);
    for (int i = 0; i < 4000; i++) anim_tick(1.0f / 240.0f);
    int retarget_converged = (fabsf_z(anims[r].position - 50.0f) < 0.5f);

    int pass = gradual && settled && converged && full_count && overflow &&
               vel_preserved && retarget_converged;
    kputs("[L1] gradual="); kput_dec((uint64_t)gradual);
    kputs(" settled=");     kput_dec((uint64_t)settled);
    kputs(" converged=");   kput_dec((uint64_t)converged);
    kputs(" concurrent=");  kput_dec((uint64_t)spawned);
    kputs(" overflow=");    kput_dec((uint64_t)overflow);
    kputs(" vel_kept=");    kput_dec((uint64_t)vel_preserved);
    kputs(" retgt_conv=");  kput_dec((uint64_t)retarget_converged);
    kputs(pass ? " -> PASS\n" : " -> FAIL\n");
    anim_init();   /* leave the pool clean for real boot */
}
#endif
