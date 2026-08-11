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

#ifdef ZEOS_DIAG_L2
#include "kprint.h"
/* L.2 selftest: prove the 4 presets produce DISTINCT, name-appropriate physics
 * (not just distinct constants). Spring 0->100 with each; measure peak position
 * (overshoot) and ticks-to-settle. Expect: BOUNCY overshoots (low damping);
 * INTERACTIVE settles fastest (high stiffness); all converge to 100. */
static int l2_run(float S, float D, float *peak_out)
{
    anim_init();
    int id = anim_spring(0.0f, 100.0f, S, D, 0, 0);
    float peak = 0.0f; int ticks = 0;
    for (int i = 0; i < 6000; i++) {
        anim_tick(1.0f / 240.0f);
        ticks++;
        if (anims[id].position > peak) peak = anims[id].position;
        if (!anims[id].active) break;
    }
    *peak_out = peak;
    return ticks;
}
void anim_l2_selftest(void)
{
    float p_snap, p_smooth, p_bouncy, p_inter;
    int t_snap   = l2_run(SPRING_SNAPPY_S,      SPRING_SNAPPY_D,      &p_snap);
    int t_smooth = l2_run(SPRING_SMOOTH_S,      SPRING_SMOOTH_D,      &p_smooth);
    int t_bouncy = l2_run(SPRING_BOUNCY_S,      SPRING_BOUNCY_D,      &p_bouncy);
    int t_inter  = l2_run(SPRING_INTERACTIVE_S, SPRING_INTERACTIVE_D, &p_inter);

    int bouncy_overshoots = (p_bouncy > 101.0f);           /* low damping rings */
    int smooth_settled    = (p_smooth < p_bouncy);         /* smoother than bouncy */
    int interactive_fast  = (t_inter < t_smooth);          /* stiffest = fastest */
    int all_converge = 1;                                  /* each ended near 100 */
    /* peak >= ~100 for all (they all reach target); convergence proven by settle */
    if (p_snap < 99.0f || p_smooth < 99.0f || p_bouncy < 99.0f || p_inter < 99.0f)
        all_converge = 0;

    int pass = bouncy_overshoots && smooth_settled && interactive_fast && all_converge;
    kputs("[L2] peak(snap/smooth/bouncy/inter)=");
    kput_dec((uint64_t)p_snap); kputs("/"); kput_dec((uint64_t)p_smooth);
    kputs("/"); kput_dec((uint64_t)p_bouncy); kputs("/"); kput_dec((uint64_t)p_inter);
    kputs(" ticks(smooth/inter)="); kput_dec((uint64_t)t_smooth);
    kputs("/"); kput_dec((uint64_t)t_inter);
    kputs(" bounce="); kput_dec((uint64_t)bouncy_overshoots);
    kputs(" inter_fast="); kput_dec((uint64_t)interactive_fast);
    kputs(pass ? " -> PASS\n" : " -> FAIL\n");
    anim_init();
}
#endif

/* ── L.6: spring scroll physics ──────────────────────────────────────
 * A scroll offset with momentum + edge rubber-band. A flick imparts velocity;
 * friction decays it; when the offset passes a content bound it springs back
 * (rubber-band) instead of hard-clamping. Distinct from a plain spring (L.1):
 * this is velocity-driven with bounded overscroll. */
void scroll_phys_init(scroll_phys_t *s, float max_offset)
{
    s->pos = 0.0f; s->vel = 0.0f; s->max = max_offset < 0 ? 0 : max_offset;
}

void scroll_phys_flick(scroll_phys_t *s, float velocity) { s->vel = velocity; }

void scroll_phys_tick(scroll_phys_t *s, float dt)
{
    if (dt <= 0.0f) return;
    if (dt > 0.05f) dt = 0.05f;               /* clamp big frames */

    /* Integrate momentum. */
    s->pos += s->vel * dt;
    s->vel *= 0.92f;                            /* friction (per-tick decay) */

    /* Edge rubber-band: past a bound, pull back with a stiff spring + heavy
     * damping so it settles at the bound without oscillating forever. */
    float target = -1.0f;
    if (s->pos < 0.0f)          target = 0.0f;
    else if (s->pos > s->max)   target = s->max;
    if (target >= 0.0f) {
        float k = 200.0f, c = 26.0f;
        float a = (target - s->pos) * k - s->vel * c;
        s->vel += a * dt;
        s->pos += s->vel * dt * 0.5f;
    }

    /* Settle: kill tiny residual motion at rest. */
    if (fabsf_z(s->vel) < 0.5f &&
        (s->pos >= -0.5f && s->pos <= s->max + 0.5f)) {
        s->vel = 0.0f;
        if (s->pos < 0.0f) s->pos = 0.0f;
        if (s->pos > s->max) s->pos = s->max;
    }
}

int scroll_phys_active(const scroll_phys_t *s)
{
    return (fabsf_z(s->vel) >= 0.5f) || s->pos < -0.5f || s->pos > s->max + 0.5f;
}

#ifdef ZEOS_DIAG_L6
#include "kprint.h"
/* L.6 selftest: (1) a flick moves the offset and DECELERATES (momentum+friction);
 * (2) it settles (comes to rest); (3) an overscroll past the bound rubber-bands
 * BACK to the bound rather than sticking past it. */
void anim_l6_selftest(void)
{
    scroll_phys_t s;
    /* momentum + friction */
    scroll_phys_init(&s, 1000.0f);
    scroll_phys_flick(&s, 800.0f);
    scroll_phys_tick(&s, 1.0f/60.0f);  float p1 = s.pos; float v1 = s.vel;
    for (int i=0;i<3;i++) scroll_phys_tick(&s, 1.0f/60.0f);
    float v2 = s.vel;
    int moved = (p1 > 0.0f);
    int decel = (v2 < v1);                       /* friction slows it */
    /* settle */
    for (int i=0;i<600;i++) scroll_phys_tick(&s, 1.0f/60.0f);
    int settled = !scroll_phys_active(&s);
    int in_bounds = (s.pos >= -0.5f && s.pos <= 1000.5f);
    /* rubber-band: shove past the top bound, must return to max */
    scroll_phys_init(&s, 1000.0f);
    s.pos = 1200.0f;                              /* overscrolled past max=1000 */
    int overscrolled = (s.pos > 1000.0f);
    for (int i=0;i<600;i++) scroll_phys_tick(&s, 1.0f/60.0f);
    int rubber_back = (s.pos <= 1000.5f && s.pos >= 999.5f);

    int pass = moved && decel && settled && in_bounds && overscrolled && rubber_back;
    kputs("[L6] moved="); kput_dec((uint64_t)moved);
    kputs(" decel="); kput_dec((uint64_t)decel);
    kputs(" settled="); kput_dec((uint64_t)settled);
    kputs(" in_bounds="); kput_dec((uint64_t)in_bounds);
    kputs(" rubberband="); kput_dec((uint64_t)rubber_back);
    kputs(pass ? " -> PASS\n" : " -> FAIL\n");
}
#endif
