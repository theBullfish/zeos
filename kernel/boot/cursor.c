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

void cursor_press(void) {
    g_cursor.pressed = 1;

    switch (g_cursor.click_anim) {
    case CLICK_ANIM_SCALE:
        /* Scale down with snappy spring */
        if (g_cursor.anim_scale >= 0)
            anim_retarget(g_cursor.anim_scale, SCALE_PRESSED);
        else
            g_cursor.anim_scale = anim_spring(
                SCALE_NORMAL, SCALE_PRESSED,
                CLICK_SCALE_STIFFNESS, CLICK_SCALE_DAMPING,
                on_scale_update, 0
            );
        break;

    case CLICK_ANIM_RIPPLE:
        /* Scale + ripple */
        if (g_cursor.anim_scale >= 0)
            anim_retarget(g_cursor.anim_scale, SCALE_PRESSED);
        else
            g_cursor.anim_scale = anim_spring(
                SCALE_NORMAL, SCALE_PRESSED,
                CLICK_SCALE_STIFFNESS, CLICK_SCALE_DAMPING,
                on_scale_update, 0
            );
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
        if (g_cursor.anim_scale >= 0)
            anim_retarget(g_cursor.anim_scale, SCALE_NORMAL);
        else
            g_cursor.anim_scale = anim_spring(
                SCALE_PRESSED, SCALE_NORMAL,
                CLICK_SCALE_STIFFNESS, CLICK_SCALE_DAMPING,
                on_scale_update, 0
            );
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
