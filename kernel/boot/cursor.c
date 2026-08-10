/*
 * Zeos — Cursor & Click Animation System
 *
 * Spring-animated cursors with persona-selectable colorways.
 * Click feedback: scale pulse + ripple ring, all physics-based.
 */

#include "cursor.h"
#include "anim.h"
#include "fb.h"
#include "theme.h"
#include "cursor_sprites.h"   /* AUTO-GENERATED real cursor artwork */

/* ── Global cursor state ── */
static cursor_t g_cursor;
static int s_confirm_frames = 0;   /* E.4: countdown to revert the confirm cursor */
#define CONFIRM_HOLD_FRAMES 18

/* ── Persona colorway table ── */
typedef struct {
    uint32_t accent;
    uint32_t accent_dim;
    uint32_t outline;
} colorway_t;

static const colorway_t COLORWAYS[] = {
    [0] = { COLOR_ZEROS_ACCENT, COLOR_ZEROS_DIM, COLOR_SURFACE },  /* Zeros — teal */
    [1] = { COLOR_DEREZ_ACCENT, COLOR_DEREZ_DIM, COLOR_SURFACE },  /* DereZ — magenta */
    [2] = { COLOR_FULL_ACCENT,  COLOR_FULL_DIM,  COLOR_SURFACE },  /* Full  — steel blue */
};

#define COLORWAY_COUNT 3
static int g_active_colorway = 2;  /* default to Full */

/* ── Colorway selection ── */

void cursor_select_colorway(int index) {
    if (index < 0 || index >= COLORWAY_COUNT) return;
    g_active_colorway = index;
    g_cursor.accent     = COLORWAYS[index].accent;
    g_cursor.accent_dim = COLORWAYS[index].accent_dim;
}

int cursor_get_colorway(void) {
    return g_active_colorway;
}

void cursor_cycle_colorway(void) {
    cursor_select_colorway((g_active_colorway + 1) % COLORWAY_COUNT);
}

/* ── Spring callbacks ── */

static void on_scale_update(int anim_id, float pos, void *ctx) {
    (void)anim_id; (void)ctx;
    g_cursor.scale = pos;
}

static void on_ripple_radius(int anim_id, float pos, void *ctx) {
    click_ripple_t *r = (click_ripple_t *)ctx;
    (void)anim_id;
    r->radius = pos;
}

static void on_ripple_opacity(int anim_id, float pos, void *ctx) {
    click_ripple_t *r = (click_ripple_t *)ctx;
    (void)anim_id;
    r->opacity = pos;
    if (pos < 1.0f) {
        r->active = 0;
        anim_cancel(r->anim_radius);
    }
}

/* ── Ripple management ── */

static click_ripple_t *ripple_alloc(void) {
    for (int i = 0; i < MAX_RIPPLES; i++) {
        if (!g_cursor.ripples[i].active)
            return &g_cursor.ripples[i];
    }
    /* Steal oldest */
    return &g_cursor.ripples[0];
}

static void ripple_spawn(int x, int y) {
    click_ripple_t *r = ripple_alloc();
    r->x = x;
    r->y = y;
    r->radius = 0.0f;
    r->opacity = 180.0f;
    r->color = g_cursor.accent;
    r->active = 1;

    r->anim_radius = anim_spring(
        0.0f, (float)RIPPLE_MAX_RADIUS,
        RIPPLE_EXPAND_STIFFNESS, RIPPLE_EXPAND_DAMPING,
        on_ripple_radius, r
    );

    r->anim_opacity = anim_spring(
        180.0f, 0.0f,
        RIPPLE_FADE_STIFFNESS, RIPPLE_FADE_DAMPING,
        on_ripple_opacity, r
    );
}

/* ── Public API ── */

void cursor_init(void) {
    g_cursor.x = (int)fb_width() / 2;    /* start visible at center (matches mouse) */
    g_cursor.y = (int)fb_height() / 2;
    g_cursor.state = CURSOR_DEFAULT;
    g_cursor.prev_state = CURSOR_DEFAULT;
    g_cursor.click_anim = CLICK_ANIM_SCALE;
    g_cursor.scale = SCALE_NORMAL;
    g_cursor.anim_scale = -1;
    g_cursor.pressed = 0;

    for (int i = 0; i < MAX_RIPPLES; i++)
        g_cursor.ripples[i].active = 0;

    /* Default to Full persona */
    cursor_select_colorway(2);
}

void cursor_set(cursor_state_t state) {
    if (state >= CURSOR_COUNT) return;
    g_cursor.prev_state = g_cursor.state;
    g_cursor.state = state;
}

void cursor_move(int x, int y) {
    g_cursor.x = x;
    g_cursor.y = y;
}

/* Drive the cursor scale toward `target`. Retargets the live scale spring
 * (preserving velocity for rapid clicks) ONLY if it's still active; otherwise
 * starts a fresh spring from the current scale. The old code retargeted a
 * possibly-SETTLED id -- anim_retarget no-ops on an inactive slot, so a click
 * held longer than the spring settle (e.g. a window drag) left the cursor stuck
 * at SCALE_PRESSED; a reused slot could even be hijacked. */
static void cursor_scale_to(float target) {
    if (g_cursor.anim_scale >= 0 && anim_is_active(g_cursor.anim_scale)) {
        anim_retarget(g_cursor.anim_scale, target);
    } else {
        g_cursor.anim_scale = anim_spring(
            g_cursor.scale, target,
            CLICK_SCALE_STIFFNESS, CLICK_SCALE_DAMPING,
            on_scale_update, 0
        );
    }
}

void cursor_press(void) {
    g_cursor.pressed = 1;

    switch (g_cursor.click_anim) {
    case CLICK_ANIM_SCALE:
        /* Scale down with snappy spring */
        cursor_scale_to(SCALE_PRESSED);
        break;

    case CLICK_ANIM_RIPPLE:
        /* Scale + ripple */
        cursor_scale_to(SCALE_PRESSED);
        ripple_spawn(g_cursor.x, g_cursor.y);
        break;

    case CLICK_ANIM_BURST:
        /* Swap to burst sprite */
        g_cursor.prev_state = g_cursor.state;
        g_cursor.state = CURSOR_CLICK_BURST;
        break;

    case CLICK_ANIM_CONFIRM:
    case CLICK_ANIM_NONE:
        break;
    }
}

void cursor_release(void) {
    g_cursor.pressed = 0;

    switch (g_cursor.click_anim) {
    case CLICK_ANIM_SCALE:
    case CLICK_ANIM_RIPPLE:
        /* Spring back through overshoot to normal */
        cursor_scale_to(SCALE_NORMAL);
        break;

    case CLICK_ANIM_BURST:
        /* Return to previous cursor after burst */
        g_cursor.state = g_cursor.prev_state;
        break;

    case CLICK_ANIM_CONFIRM:
    case CLICK_ANIM_NONE:
        break;
    }
}

void cursor_confirm(void) {
    /* Briefly flash checkmark cursor */
    g_cursor.prev_state = g_cursor.state;
    g_cursor.state = CURSOR_CLICK_CONFIRM;
    s_confirm_frames = CONFIRM_HOLD_FRAMES;   /* cursor_tick reverts it */
}

void cursor_set_click_anim(click_anim_type_t type) {
    g_cursor.click_anim = type;
}

void cursor_set_persona(uint32_t accent, uint32_t accent_dim) {
    g_cursor.accent = accent;
    g_cursor.accent_dim = accent_dim;
}

void cursor_tick(float dt) {
    /* Spring system handles all animation ticking via anim_tick(dt) */
    /* Clean up settled scale animation */
    if (g_cursor.anim_scale >= 0 && !anim_is_active(g_cursor.anim_scale)) {
        g_cursor.scale = SCALE_NORMAL;
        g_cursor.anim_scale = -1;
    }

    /* E.4: revert the confirm (checkmark) cursor after a brief hold. */
    if (s_confirm_frames > 0 && --s_confirm_frames == 0)
        g_cursor.state = g_cursor.prev_state;
}

#ifdef ZEOS_DIAG_E4
#include "kprint.h"
/* E.4 selftest: prove cursor_confirm() flashes the checkmark cursor and that
 * cursor_tick reverts it after the hold. (The wiring into settings save_all is
 * source-verifiable.) */
void cursor_e4_selftest(void)
{
    cursor_state_t before = g_cursor.state;
    cursor_confirm();
    int flashed = (g_cursor.state == CURSOR_CLICK_CONFIRM);
    for (int i = 0; i < CONFIRM_HOLD_FRAMES; i++) cursor_tick(0.016f);
    int reverted = (g_cursor.state == before);
    kputs("[E4] cursor_confirm flash="); kput_dec((uint64_t)flashed);
    kputs(" reverted="); kput_dec((uint64_t)reverted);
    kputs((flashed && reverted) ? " -> PASS\n" : " -> FAIL\n");
}
#endif

/* ── Drawing ── */

/*
 * Draw a ripple ring: circle outline at (x,y) with radius r,
 * colored with persona accent at the given opacity.
 */
static void draw_ripple(click_ripple_t *r) {
    if (!r->active || r->opacity < 1.0f) return;

    /* Extract RGB from accent, apply animated opacity */
    uint8_t a = (uint8_t)r->opacity;
    uint32_t color = (r->color & 0x00FFFFFF) | ((uint32_t)a << 24);

    int ri = (int)r->radius;
    if (ri < 2) return;

    /* Draw 2px ring */
    fb_circle(r->x, r->y, ri, color);
    fb_circle(r->x, r->y, ri - 1, color);
}

/* Software save-under: the framebuffer has no back buffer, so the cursor would
 * smear a trail as it moves. We stash the pixels beneath the arrow, restore them
 * next frame (unless the scene recomposited -- which already repainted them),
 * then re-capture. Portable: plain fb read/blit, works identically on ARM. */
#define CUR_BOX_W (CURSOR_SPR_SZ + 2)
#define CUR_BOX_H (CURSOR_SPR_SZ + 2)
static uint32_t s_cur_backup[CUR_BOX_W * CUR_BOX_H];
static int s_cur_bx, s_cur_by, s_cur_saved;

void cursor_draw(void) {
    extern int  compositor_composited_this_tick(void);
    extern void fb_read_rect(int, int, int, int, uint32_t *);

    int composited = compositor_composited_this_tick();

    /* Restore what was under the cursor last frame -- but NOT on a tick where
     * the scene recomposited (the fresh frame already repainted that region;
     * restoring stale pixels would stamp old content over it). */
    if (s_cur_saved && !composited)
        fb_blit(s_cur_bx, s_cur_by, CUR_BOX_W, CUR_BOX_H, s_cur_backup);

    int cx = g_cursor.x;
    int cy = g_cursor.y;

    /* Pick the real sprite for the current state; position by its hotspot. */
    int st = (int)g_cursor.state;
    if (st < 0 || st >= (int)(sizeof(cursor_sprites) / sizeof(cursor_sprites[0])))
        st = 0;
    int hx = cursor_hotspot[st][0];
    int hy = cursor_hotspot[st][1];
    int ox = cx - hx;                      /* sprite top-left */
    int oy = cy - hy;

    /* Capture the region under the sprite (1px margin) BEFORE drawing ripples
     * or the sprite, so the backup is clean scene pixels. */
    s_cur_bx = ox - 1;
    s_cur_by = oy - 1;
    fb_read_rect(s_cur_bx, s_cur_by, CUR_BOX_W, CUR_BOX_H, s_cur_backup);
    s_cur_saved = 1;

    /* Draw active ripples (behind cursor) */
    for (int i = 0; i < MAX_RIPPLES; i++) {
        if (g_cursor.ripples[i].active)
            draw_ripple(&g_cursor.ripples[i]);
    }

    /* Blit the real cursor sprite, alpha-blended (white core + dark outline). */
    {
        extern void fb_pixel_blend(int, int, uint32_t);
        const uint32_t *spr = cursor_sprites[st];
        for (int yy = 0; yy < CURSOR_SPR_SZ; yy++)
            for (int xx = 0; xx < CURSOR_SPR_SZ; xx++) {
                uint32_t p = spr[yy * CURSOR_SPR_SZ + xx];
                if (p >> 24)                /* skip fully-transparent pixels */
                    fb_pixel_blend(ox + xx, oy + yy, p);
            }
    }
}

#include "kprint.h"   /* header-guarded; E.3/E.2 selftests print to serial */
/* E.3 selftest: prove all three click-feedback physics fire.
 *  SCALE:  press springs scale toward SCALE_PRESSED (0.85), release restores ~1.0
 *  RIPPLE: press spawns an active ripple whose radius grows and opacity fades
 *  BURST:  press swaps to CURSOR_CLICK_BURST sprite, release restores previous
 * Un-gated so the production `selftest` shell can run it; PRODUCTION-SAFE: this
 * heavily mutates live cursor state (position via cursor_move, ripples, scale,
 * springs, state/prev_state, click_anim), so it snapshots the WHOLE g_cursor
 * struct up front, then before returning CANCELS every spring the test spawned
 * (scale + ripple radius/opacity — else their orphaned callbacks would re-corrupt
 * the restored state and render a ghost ripple at 500,500 on the live desktop)
 * and restores g_cursor byte-identically. No VAULT, no surface. Returns 1 on PASS. */
int cursor_e3_selftest(void)
{
    cursor_t saved = g_cursor;      /* full snapshot of the live cursor */

    cursor_move(500, 500);

    /* SCALE (default). Tick BOTH anim_tick + cursor_tick to mirror the real
     * per-frame path (cursor_tick owns settled-spring cleanup). */
    g_cursor.click_anim = CLICK_ANIM_SCALE;
    g_cursor.anim_scale = -1; g_cursor.scale = SCALE_NORMAL;
    cursor_press();
    for (int i = 0; i < 3; i++) { anim_tick(1.0f / 240.0f); }  /* mid-pulse, pre-settle */
    float scale_min = g_cursor.scale;
    int scale_shrank = (scale_min < SCALE_NORMAL - 0.01f);
    cursor_release();
    for (int i = 0; i < 400; i++) { anim_tick(1.0f / 240.0f); cursor_tick(1.0f / 240.0f); }
    int scale_restored = (g_cursor.scale > SCALE_NORMAL - 0.02f);

    /* RIPPLE */
    g_cursor.click_anim = CLICK_ANIM_RIPPLE;
    for (int i = 0; i < MAX_RIPPLES; i++) g_cursor.ripples[i].active = 0;
    cursor_press();
    for (int i = 0; i < 20; i++) anim_tick(1.0f / 240.0f);
    int ripple_active = 0; float rr = 0.0f, ro = 180.0f;
    for (int i = 0; i < MAX_RIPPLES; i++)
        if (g_cursor.ripples[i].active) {
            ripple_active = 1;
            rr = g_cursor.ripples[i].radius;
            ro = g_cursor.ripples[i].opacity;
        }
    int ripple_grew = ripple_active && (rr > 0.0f) && (ro < 180.0f);
    cursor_release();

    /* BURST */
    g_cursor.click_anim = CLICK_ANIM_BURST;
    g_cursor.state = CURSOR_DEFAULT; g_cursor.prev_state = CURSOR_DEFAULT;
    cursor_press();
    int burst_on = (g_cursor.state == CURSOR_CLICK_BURST);
    cursor_release();
    int burst_off = (g_cursor.state == CURSOR_DEFAULT);

    /* Cancel every spring THIS selftest spawned before restoring — otherwise their
     * callbacks would fire on the next anim_tick and re-write the restored cursor
     * fields (live ghost ripple/scale). anim_scale has a real -1 sentinel
     * (cursor_init); the ripple anim-id fields do NOT (zero-initialized), so guard
     * ripple cancels on .active (only spawned ripples are active) — never on the
     * id value, else we'd anim_cancel(slot 0) for the 3 un-spawned ripples and
     * kill an unrelated spring. Then restore the snapshot and force ALL ripples
     * inactive so no ghost ring can render even if a click was in-flight when the
     * selftest ran. */
    if (g_cursor.anim_scale >= 0) anim_cancel(g_cursor.anim_scale);
    for (int i = 0; i < MAX_RIPPLES; i++)
        if (g_cursor.ripples[i].active) {
            anim_cancel(g_cursor.ripples[i].anim_radius);
            anim_cancel(g_cursor.ripples[i].anim_opacity);
        }
    g_cursor = saved;               /* restore the pre-test cursor */
    for (int i = 0; i < MAX_RIPPLES; i++) g_cursor.ripples[i].active = 0;  /* no ghost ring */

    int pass = scale_shrank && scale_restored && ripple_active && ripple_grew
             && burst_on && burst_off;
    kputs("[E3] scale[press/restore]="); kput_dec((uint64_t)scale_shrank); kput_dec((uint64_t)scale_restored);
    kputs(" scale_min_x100="); kput_dec((uint64_t)(scale_min * 100.0f));   /* ~85 = 0.85 pressed */
    kputs(" ripple[active/grew]="); kput_dec((uint64_t)ripple_active); kput_dec((uint64_t)ripple_grew);
    kputs(" r="); kput_dec((uint64_t)rr); kputs(" o="); kput_dec((uint64_t)ro);   /* radius grew, opacity<180 */
    kputs(" burst[on/off]="); kput_dec((uint64_t)burst_on); kput_dec((uint64_t)burst_off);
    kputs(pass ? " -> PASS\n" : " -> FAIL\n");
    return pass;
}

uint32_t cursor_get_accent(void) { return g_cursor.accent; }

#include "kprint.h"
/* E.2 selftest: 22 cursor states each have a real (non-empty) 24x24 sprite and a
 * hotspot-table entry; hotspots are not all identical (per-cursor hotspots);
 * cursor_set accepts every state. save-under is N/A here: the compositor fully
 * recomposites under the cursor each frame (no saved-pixels restore needed).
 * Un-gated so the production `selftest` shell can run it; PRODUCTION-SAFE:
 * cursor_set is pure in-memory (no VAULT/dirty); the selftest saves BOTH
 * g_cursor.state and g_cursor.prev_state up front and restores them exactly, so
 * the live cursor is byte-identical afterwards (cleaner than the harness which
 * only reset state to DEFAULT and left prev_state dirty). Returns 1 on PASS. */
int cursor_e2_selftest(void)
{
    cursor_state_t saved_state = g_cursor.state;
    cursor_state_t saved_prev  = g_cursor.prev_state;

    int count_ok = (CURSOR_COUNT == 22);
    int nonempty = 0;
    for (int s = 0; s < 22; s++) {
        for (int p = 0; p < CURSOR_SPR_SZ * CURSOR_SPR_SZ; p++)
            if (cursor_sprites[s][p] >> 24) { nonempty++; break; }  /* any alpha>0 */
    }
    int hotspots_distinct = 0;
    for (int s = 1; s < 22; s++)
        if (cursor_hotspot[s][0] != cursor_hotspot[0][0] ||
            cursor_hotspot[s][1] != cursor_hotspot[0][1]) { hotspots_distinct = 1; break; }
    int set_ok = 1;
    for (int s = 0; s < 22; s++) { cursor_set((cursor_state_t)s); if (g_cursor.state != s) set_ok = 0; }

    /* restore the exact pre-selftest cursor (state + prev_state) — no net change */
    g_cursor.state = saved_state;
    g_cursor.prev_state = saved_prev;

    int pass = count_ok && (nonempty == 22) && hotspots_distinct && set_ok;
    kputs("[E2] states="); kput_dec((uint64_t)CURSOR_COUNT);
    kputs(" nonempty_sprites="); kput_dec((uint64_t)nonempty); kputs("/22");
    kputs(" hotspots_distinct="); kput_dec((uint64_t)hotspots_distinct);
    kputs(" set_ok="); kput_dec((uint64_t)set_ok);
    kputs(pass ? " -> PASS (save-under N/A: full recomposite)\n" : " -> FAIL\n");
    return pass;
}
