/*
 * Zeos — Persona Transition Animation
 *
 * Crossfades between two persona accent colors using a spring-animated
 * t value (0.0 = old persona, 1.0 = new persona). The spring uses
 * SPRING_SMOOTH (100/16) for a gentle, satisfying transition (~300ms).
 *
 * Color interpolation: per-channel linear interpolation (lerp) between
 * source and target colors, driven by the spring position.
 *
 * When the spring settles:
 *   - Cursor colorway is applied
 *   - MasQ persona filter is applied
 *   - Panel persona is applied
 *   - Compositor is marked dirty
 *   - Transition is marked done
 *
 * Bare-metal: no stdlib, no math.h.
 */

#include "persona_anim.h"
#include "kprint.h"
#include "anim.h"
#include "theme.h"
#include "persona.h"
#include "cursor.h"
#include "persona_filter.h"
#include "panel.h"
#include "compositor.h"

/* ── Color tables (indexed by persona enum) ── */

static const uint32_t accent_table[] = {
    COLOR_ZEROS_ACCENT,   /* PERSONA_ZEROS */
    COLOR_DEREZ_ACCENT,   /* PERSONA_DEREZ */
    COLOR_FULL_ACCENT,    /* PERSONA_FULL */
};

static const uint32_t dim_table[] = {
    COLOR_ZEROS_DIM,      /* PERSONA_ZEROS */
    COLOR_DEREZ_DIM,      /* PERSONA_DEREZ */
    COLOR_FULL_DIM,       /* PERSONA_FULL */
};

/* ── Transition state ── */

static struct {
    int      active;          /* 1 = transition in progress */
    int      anim_id;         /* spring animation id */
    float    t;               /* current interpolation factor (0.0 → 1.0) */

    /* Source colors (from_persona) */
    uint32_t src_accent;
    uint32_t src_dim;

    /* Target colors (to_persona) */
    uint32_t dst_accent;
    uint32_t dst_dim;

    /* Target persona (applied when spring settles) */
    int      to_persona;
} g_ptrans = {
    .active  = 0,
    .anim_id = -1,
    .t       = 0.0f,
};

/* ── Channel extraction helpers ── */

static inline uint8_t ch_r(uint32_t c) { return (uint8_t)((c >> 16) & 0xFF); }
static inline uint8_t ch_g(uint32_t c) { return (uint8_t)((c >>  8) & 0xFF); }
static inline uint8_t ch_b(uint32_t c) { return (uint8_t)((c      ) & 0xFF); }
static inline uint8_t ch_a(uint32_t c) { return (uint8_t)((c >> 24) & 0xFF); }

/* ── Color lerp ── */

static uint32_t color_lerp(uint32_t from, uint32_t to, float t)
{
    /* Clamp t to [0, 1] */
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    uint8_t r = (uint8_t)((float)ch_r(from) + ((float)ch_r(to) - (float)ch_r(from)) * t);
    uint8_t g = (uint8_t)((float)ch_g(from) + ((float)ch_g(to) - (float)ch_g(from)) * t);
    uint8_t b = (uint8_t)((float)ch_b(from) + ((float)ch_b(to) - (float)ch_b(from)) * t);
    uint8_t a = (uint8_t)((float)ch_a(from) + ((float)ch_a(to) - (float)ch_a(from)) * t);

    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* ── Spring callback ── */

static void on_transition_update(int anim_id, float position, void *ctx)
{
    (void)anim_id;
    (void)ctx;
    g_ptrans.t = position;

    /*
     * Mark compositor dirty each frame during transition so the
     * interpolated accent color is picked up by all rendering.
     */
    compositor_dirty_all();

    /*
     * Check if the spring has settled (position == target == 1.0).
     * The anim system snaps to target and deactivates when settled,
     * so this final callback fires at exactly 1.0.
     */
    if (position >= 1.0f) {
        /* Spring settled — apply final persona settings */
        int p = g_ptrans.to_persona;

        /* Cursor colorway: 0=zeros, 1=derez, 2=full */
        cursor_select_colorway(p);

        /* MasQ perception filter */
        persona_set_active(p);

        /* Panel accent */
        panel_set_persona(p);

        /* Force full redraw with final colors */
        compositor_dirty_all();

        /* Transition complete */
        g_ptrans.active = 0;
        g_ptrans.anim_id = -1;
    }
}

/* ── Public API ── */

void persona_transition(int from_persona, int to_persona)
{
    /* Validate persona indices */
    if (from_persona < PERSONA_ZEROS || from_persona > PERSONA_FULL)
        from_persona = PERSONA_FULL;
    if (to_persona < PERSONA_ZEROS || to_persona > PERSONA_FULL)
        to_persona = PERSONA_FULL;

    /* Same persona — no transition needed */
    if (from_persona == to_persona)
        return;

    /* If a transition is already running, cancel it and start fresh */
    if (g_ptrans.active && g_ptrans.anim_id >= 0) {
        anim_cancel(g_ptrans.anim_id);
    }

    /* Store source and target colors */
    g_ptrans.src_accent = accent_table[from_persona];
    g_ptrans.src_dim    = dim_table[from_persona];
    g_ptrans.dst_accent = accent_table[to_persona];
    g_ptrans.dst_dim    = dim_table[to_persona];
    g_ptrans.to_persona = to_persona;
    g_ptrans.t          = 0.0f;
    g_ptrans.active     = 1;

    /* Start spring: 0.0 → 1.0 with SPRING_SMOOTH (100/16) */
    g_ptrans.anim_id = anim_spring(
        0.0f, 1.0f,
        SPRING_SMOOTH_S, SPRING_SMOOTH_D,
        on_transition_update, 0
    );

    /* If anim pool is full, apply immediately (fallback) */
    if (g_ptrans.anim_id < 0) {
        g_ptrans.active = 0;
        g_ptrans.t = 1.0f;
        cursor_select_colorway(to_persona);
        persona_set_active(to_persona);
        panel_set_persona(to_persona);
        compositor_dirty_all();
    }
}

uint32_t persona_current_accent(void)
{
    if (!g_ptrans.active)
        return g_ptrans.dst_accent;

    return color_lerp(g_ptrans.src_accent, g_ptrans.dst_accent, g_ptrans.t);
}

uint32_t persona_current_accent_dim(void)
{
    if (!g_ptrans.active)
        return g_ptrans.dst_dim;

    return color_lerp(g_ptrans.src_dim, g_ptrans.dst_dim, g_ptrans.t);
}

int persona_transitioning(void)
{
    return g_ptrans.active;
}

void persona_anim_tick(void)
{
    /*
     * The spring is ticked by the global anim_tick() in the compositor
     * frame loop, so there's nothing extra to drive here. This function
     * exists as a hook point if we ever need per-frame transition logic
     * beyond what the spring callback provides (e.g., particle effects,
     * secondary animations keyed to the transition progress).
     */
    (void)0;
}

#ifdef ZEOS_DIAG_G3
/* G.3 selftest: persona crossfade is a real per-channel spring color lerp, not a
 * hard swap. Transition ZEROS->FULL: mid-flight accent differs from BOTH
 * endpoints (proves lerp); settles exactly on FULL's accent; transitioning
 * flag goes 1 then 0. */
void persona_g3_selftest(void)
{
    extern void anim_tick(float);
    uint32_t src = accent_table[PERSONA_ZEROS];
    uint32_t dst = accent_table[PERSONA_FULL];

    persona_transition(PERSONA_ZEROS, PERSONA_FULL);
    int started = persona_transitioning();
    for (int i = 0; i < 8; i++) anim_tick(1.0f / 240.0f);
    uint32_t mid = persona_current_accent();
    int is_lerp = (mid != src) && (mid != dst);          /* genuine intermediate */
    for (int i = 0; i < 2000; i++) anim_tick(1.0f / 240.0f);
    int settled = (persona_transitioning() == 0);
    uint32_t fin = persona_current_accent();
    int converged = (fin == dst);

    int pass = started && is_lerp && settled && converged;
    kputs("[G3] src="); kput_hex(src); kputs(" mid="); kput_hex(mid);
    kputs(" dst="); kput_hex(dst);
    kputs(" started="); kput_dec((uint64_t)started);
    kputs(" lerp="); kput_dec((uint64_t)is_lerp);
    kputs(" settled="); kput_dec((uint64_t)settled);
    kputs(" converged="); kput_dec((uint64_t)converged);
    kputs(pass ? " -> PASS\n" : " -> FAIL\n");
}
#endif

#ifdef ZEOS_DIAG_G12
#include "persona.h"
/* G.1: 3 personas have distinct accent AND dim tokens (each dim != its accent).
 * G.2: shell prompt differs per persona AND cursor colorway switches per persona. */
void persona_g12_selftest(void)
{
    extern void cursor_select_colorway(int);
    extern uint32_t cursor_get_accent(void);
    extern int str_eq_z(const char*, const char*);  /* not used; compare inline */

    /* G.1 tokens distinct */
    int acc_distinct = (accent_table[0] != accent_table[1]) &&
                       (accent_table[1] != accent_table[2]) &&
                       (accent_table[0] != accent_table[2]);
    int dim_distinct = (dim_table[0] != dim_table[1]) &&
                       (dim_table[1] != dim_table[2]) &&
                       (dim_table[0] != dim_table[2]);
    int dim_ne_acc = (dim_table[0] != accent_table[0]) &&
                     (dim_table[1] != accent_table[1]) &&
                     (dim_table[2] != accent_table[2]);

    /* G.2 prompts distinct (compare first chars: 'z'eros/'d'erez/'z'eos -> compare full) */
    const char *p0 = persona_prompt(PERSONA_ZEROS);
    const char *p1 = persona_prompt(PERSONA_DEREZ);
    const char *p2 = persona_prompt(PERSONA_FULL);
    /* inline strcmp */
    int prompt_distinct = 1;
    { const char *a=p0,*b=p1; while(*a&&*b&&*a==*b){a++;b++;} if(*a==*b) prompt_distinct=0; }
    { const char *a=p1,*b=p2; while(*a&&*b&&*a==*b){a++;b++;} if(*a==*b) prompt_distinct=0; }
    { const char *a=p0,*b=p2; while(*a&&*b&&*a==*b){a++;b++;} if(*a==*b) prompt_distinct=0; }

    /* G.2 cursor colorway switches per persona */
    cursor_select_colorway(PERSONA_ZEROS); uint32_t c0 = cursor_get_accent();
    cursor_select_colorway(PERSONA_DEREZ); uint32_t c1 = cursor_get_accent();
    cursor_select_colorway(PERSONA_FULL);  uint32_t c2 = cursor_get_accent();
    int cursor_switches = (c0 != c1) && (c1 != c2) && (c0 != c2);
    cursor_select_colorway(PERSONA_FULL);  /* restore default */

    int pass = acc_distinct && dim_distinct && dim_ne_acc && prompt_distinct && cursor_switches;
    kputs("[G12] acc_distinct="); kput_dec((uint64_t)acc_distinct);
    kputs(" dim_distinct="); kput_dec((uint64_t)dim_distinct);
    kputs(" dim!=acc="); kput_dec((uint64_t)dim_ne_acc);
    kputs(" prompt_distinct="); kput_dec((uint64_t)prompt_distinct);
    kputs(" cursor_sw="); kput_dec((uint64_t)cursor_switches);
    kputs(" ("); kputs(p0); kputs("/"); kputs(p1); kputs("/"); kputs(p2); kputs(")");
    kputs(pass ? " -> PASS\n" : " -> FAIL\n");
}
#endif
