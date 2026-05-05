/*
 * Zeos — Window Manager
 *
 * Chain surface management: chrome, stacking, drag, resize, snap.
 * Everything spring-animated where it makes sense.
 *
 * Window snap: half / quadrant / maximize via drag-to-edge or
 * Super+arrow. Drag preview overlay, spring-animated commit,
 * pre-snap geometry stack for restore. Multi-display aware.
 */

#include "wm.h"
#include "workspaces.h"
#include "fb.h"
#include "font.h"
#include "anim.h"
#include "theme.h"
#include "kprint.h"

/* ── Global state ── */
static wm_state_t g_wm;
static int g_desktop_shown;  /* 1 = show-desktop mode active */

/* ── Snap tunables (settings.snap_drag_threshold / snap_animation_ms) ── */
static int g_snap_drag_threshold = 16;   /* pixels from edge */
static int g_snap_animation_ms   = 180;  /* total animation time hint */

/* Drag-anchor remembers where the drag started so we can cancel. */
static int g_drag_anchor_x;
static int g_drag_anchor_y;

void wm_set_snap_drag_threshold(int px) {
    if (px < 1) px = 1;
    if (px > 256) px = 256;
    g_snap_drag_threshold = px;
}
int wm_get_snap_drag_threshold(void) { return g_snap_drag_threshold; }

void wm_set_snap_animation_ms(int ms) {
    if (ms < 30)   ms = 30;
    if (ms > 2000) ms = 2000;
    g_snap_animation_ms = ms;
}
int wm_get_snap_animation_ms(void) { return g_snap_animation_ms; }

/* Map snap_animation_ms to spring constants. Shorter ms -> stiffer spring.
 * Default 180ms ≈ INTERACTIVE preset. */
static void snap_spring_constants(float *stiff, float *damp) {
    /* Stiffness scales inversely with target time. SPRING_INTERACTIVE_S
     * (400) settles in ~180ms. Below 100ms, push to a snappier preset. */
    if (g_snap_animation_ms <= 120) {
        *stiff = 600.0f; *damp = 32.0f;
    } else if (g_snap_animation_ms <= 240) {
        *stiff = SPRING_INTERACTIVE_S; *damp = SPRING_INTERACTIVE_D;
    } else {
        *stiff = SPRING_SNAPPY_S; *damp = SPRING_SNAPPY_D;
    }
}

/* ── Helpers ── */

static void str_copy(char *d, const char *s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static chain_surface_t *find_surface(int id) {
    for (int i = 0; i < g_wm.surface_count; i++) {
        if (g_wm.surfaces[i].id == id)
            return &g_wm.surfaces[i];
    }
    return 0;
}

static int next_z(void) {
    int max_z = 0;
    for (int i = 0; i < g_wm.surface_count; i++) {
        if (g_wm.surfaces[i].z_index > max_z)
            max_z = g_wm.surfaces[i].z_index;
    }
    return max_z + 1;
}

/* ── Spring animation callbacks ── */

static void surface_scale_cb(int anim_id, float position, void *ctx) {
    (void)anim_id;
    chain_surface_t *s = (chain_surface_t *)ctx;
    s->anim_scale = position;

    /* If closing and animation settled at target, remove the surface */
    if (s->closing && !anim_is_active(s->anim_scale_id)) {
        s->visible = 0;
        s->signal = SIGNAL_DETACHED;
        s->closing = 0;
    }
}

static void surface_opacity_cb(int anim_id, float position, void *ctx) {
    (void)anim_id;
    chain_surface_t *s = (chain_surface_t *)ctx;
    s->anim_opacity = position;
}

static void surface_x_cb(int anim_id, float position, void *ctx) {
    (void)anim_id;
    chain_surface_t *s = (chain_surface_t *)ctx;
    s->anim_x = position;
}

static void surface_y_cb(int anim_id, float position, void *ctx) {
    (void)anim_id;
    chain_surface_t *s = (chain_surface_t *)ctx;
    s->anim_y = position;
}

static void surface_w_cb(int anim_id, float position, void *ctx) {
    (void)anim_id;
    chain_surface_t *s = (chain_surface_t *)ctx;
    s->anim_w = position;
}

static void surface_h_cb(int anim_id, float position, void *ctx) {
    (void)anim_id;
    chain_surface_t *s = (chain_surface_t *)ctx;
    s->anim_h = position;
}

/* Cancel all geometry spring anims on a surface */
static void cancel_geom_anims(chain_surface_t *s) {
    if (s->anim_x_id >= 0) { anim_cancel(s->anim_x_id); s->anim_x_id = -1; }
    if (s->anim_y_id >= 0) { anim_cancel(s->anim_y_id); s->anim_y_id = -1; }
    if (s->anim_w_id >= 0) { anim_cancel(s->anim_w_id); s->anim_w_id = -1; }
    if (s->anim_h_id >= 0) { anim_cancel(s->anim_h_id); s->anim_h_id = -1; }
}

/* Start or retarget geometry springs to animate x/y/w/h to targets */
static void spring_geom_to(chain_surface_t *s, int tx, int ty, int tw, int th,
                           float stiffness, float damping) {
    /* X */
    if (s->anim_x_id >= 0 && anim_is_active(s->anim_x_id))
        anim_retarget(s->anim_x_id, (float)tx);
    else
        s->anim_x_id = anim_spring(s->anim_x, (float)tx, stiffness, damping,
                                    surface_x_cb, s);

    /* Y */
    if (s->anim_y_id >= 0 && anim_is_active(s->anim_y_id))
        anim_retarget(s->anim_y_id, (float)ty);
    else
        s->anim_y_id = anim_spring(s->anim_y, (float)ty, stiffness, damping,
                                    surface_y_cb, s);

    /* W */
    if (s->anim_w_id >= 0 && anim_is_active(s->anim_w_id))
        anim_retarget(s->anim_w_id, (float)tw);
    else
        s->anim_w_id = anim_spring(s->anim_w, (float)tw, stiffness, damping,
                                    surface_w_cb, s);

    /* H */
    if (s->anim_h_id >= 0 && anim_is_active(s->anim_h_id))
        anim_retarget(s->anim_h_id, (float)th);
    else
        s->anim_h_id = anim_spring(s->anim_h, (float)th, stiffness, damping,
                                    surface_h_cb, s);
}

/* ── Initialization ── */

void wm_init(int screen_w, int screen_h, int panel_h) {
    g_wm.surface_count = 0;
    g_wm.focused_id = -1;
    g_wm.next_id = 1;
    g_wm.controls_side = WM_CONTROLS_RIGHT;  /* Default, overridden by first boot */
    g_wm.active_workspace = 0;
    g_wm.screen_w = screen_w;
    g_wm.screen_h = screen_h;
    g_wm.panel_h = panel_h;
    g_wm.dock_h = 0;  /* Auto-hidden by default */
    g_wm.ghost.active = 0;
    g_desktop_shown = 0;

    for (int i = 0; i < WM_MAX_WORKSPACES; i++)
        g_wm.tiling_enabled[i] = 0;

    kputs("WM: initialized ");
    kput_dec(screen_w);
    kputs("x");
    kput_dec(screen_h);
    kputs("\n");
}

void wm_set_controls_side(wm_controls_side_t side) {
    g_wm.controls_side = side;
}

wm_controls_side_t wm_get_controls_side(void) {
    return g_wm.controls_side;
}

/* ── Surface lifecycle ── */

int wm_create_surface(const char *title, int chain_id,
                      int x, int y, int w, int h,
                      void (*draw_content)(int, int, int, int, int))
{
    if (g_wm.surface_count >= WM_MAX_SURFACES) return -1;

    chain_surface_t *s = &g_wm.surfaces[g_wm.surface_count++];
    s->id = g_wm.next_id++;
    str_copy(s->title, title, 128);
    s->chain_id = chain_id;
    s->signal = SIGNAL_LIVE;
    s->x = x;
    s->y = y;
    s->w = w < WM_MIN_SURFACE_W ? WM_MIN_SURFACE_W : w;
    s->h = h < WM_MIN_SURFACE_H ? WM_MIN_SURFACE_H : h;
    s->min_w = WM_MIN_SURFACE_W;
    s->min_h = WM_MIN_SURFACE_H;
    s->state = SURFACE_NORMAL;
    s->saved_x = x;
    s->saved_y = y;
    s->saved_w = s->w;
    s->saved_h = s->h;
    s->z_index = next_z();
    s->workspace = g_wm.active_workspace;
    s->visible = 1;
    s->focused = 0;
    s->accent = COLOR_PRIMARY;
    s->dragging = 0;
    s->resizing = 0;
    s->draw_content = draw_content;

    /* Initialize animation state */
    s->anim_scale = 0.8f;     /* Start small */
    s->anim_opacity = 0.0f;   /* Start transparent */
    s->anim_scale_id = -1;
    s->anim_opacity_id = -1;
    s->anim_x_id = -1;
    s->anim_y_id = -1;
    s->anim_w_id = -1;
    s->anim_h_id = -1;
    s->anim_x = (float)s->x;
    s->anim_y = (float)s->y;
    s->anim_w = (float)s->w;
    s->anim_h = (float)s->h;
    s->closing = 0;

    /* Open animation: scale 0.8 → 1.0, opacity 0 → 255 */
    s->anim_scale_id = anim_spring(0.8f, 1.0f,
                                    SPRING_SNAPPY_S, SPRING_SNAPPY_D,
                                    surface_scale_cb, s);
    s->anim_opacity_id = anim_spring(0.0f, 255.0f,
                                      SPRING_SNAPPY_S, SPRING_SNAPPY_D,
                                      surface_opacity_cb, s);

    wm_focus_surface(s->id);
    workspaces_window_assigned(s->id, s->workspace);

    return s->id;
}

void wm_detach_surface(int id) {
    chain_surface_t *s = find_surface(id);
    if (!s) return;

    /* Start close animation: scale 1.0 → 0.8, opacity 255 → 0 */
    s->closing = 1;

    if (s->anim_scale_id >= 0 && anim_is_active(s->anim_scale_id))
        anim_retarget(s->anim_scale_id, 0.8f);
    else
        s->anim_scale_id = anim_spring(s->anim_scale, 0.8f,
                                        SPRING_SNAPPY_S, SPRING_SNAPPY_D,
                                        surface_scale_cb, s);

    if (s->anim_opacity_id >= 0 && anim_is_active(s->anim_opacity_id))
        anim_retarget(s->anim_opacity_id, 0.0f);
    else
        s->anim_opacity_id = anim_spring(s->anim_opacity, 0.0f,
                                          SPRING_SNAPPY_S, SPRING_SNAPPY_D,
                                          surface_opacity_cb, s);

    /* If focused, focus next visible */
    if (g_wm.focused_id == id) {
        g_wm.focused_id = -1;
        int best_z = -1;
        for (int i = 0; i < g_wm.surface_count; i++) {
            chain_surface_t *o = &g_wm.surfaces[i];
            if (o->visible && !o->closing &&
                o->workspace == g_wm.active_workspace &&
                o->z_index > best_z) {
                best_z = o->z_index;
                g_wm.focused_id = o->id;
            }
        }
    }
}

void wm_minimize_surface(int id) {
    chain_surface_t *s = find_surface(id);
    if (!s) return;

    s->saved_x = s->x; s->saved_y = s->y;
    s->saved_w = s->w; s->saved_h = s->h;
    s->state = SURFACE_MINIMIZED;
    s->visible = 0;
}

void wm_restore_surface(int id) {
    chain_surface_t *s = find_surface(id);
    if (!s) return;

    s->x = s->saved_x; s->y = s->saved_y;
    s->w = s->saved_w; s->h = s->saved_h;
    s->state = SURFACE_NORMAL;
    s->visible = 1;
    wm_focus_surface(id);
}

void wm_maximize_surface(int id) {
    chain_surface_t *s = find_surface(id);
    if (!s) return;

    if (s->state == SURFACE_MAXIMIZED) {
        /* Restore — spring-animate back to saved geometry */
        s->state = SURFACE_NORMAL;
        s->x = s->saved_x; s->y = s->saved_y;
        s->w = s->saved_w; s->h = s->saved_h;
        spring_geom_to(s, s->saved_x, s->saved_y, s->saved_w, s->saved_h,
                        SPRING_SNAPPY_S, SPRING_SNAPPY_D);
    } else {
        /* Save and maximize — spring-animate to full screen */
        s->saved_x = s->x; s->saved_y = s->y;
        s->saved_w = s->w; s->saved_h = s->h;

        int tx = 0;
        int ty = g_wm.panel_h;
        int tw = g_wm.screen_w;
        int th = g_wm.screen_h - g_wm.panel_h;

        s->x = tx; s->y = ty; s->w = tw; s->h = th;
        spring_geom_to(s, tx, ty, tw, th, SPRING_SNAPPY_S, SPRING_SNAPPY_D);
        s->state = SURFACE_MAXIMIZED;
    }
}

/* ── Focus ── */

void wm_focus_surface(int id) {
    /* Unfocus previous */
    for (int i = 0; i < g_wm.surface_count; i++)
        g_wm.surfaces[i].focused = 0;

    chain_surface_t *s = find_surface(id);
    if (!s) return;

    s->focused = 1;
    s->z_index = next_z();
    g_wm.focused_id = id;
    workspaces_window_focused(id);
}

int wm_get_focused(void) {
    return g_wm.focused_id;
}

/* ── Movement / resize ── */

void wm_move_surface(int id, int x, int y) {
    chain_surface_t *s = find_surface(id);
    if (!s) return;
    s->x = x;
    s->y = y;
}

void wm_resize_surface(int id, int w, int h) {
    chain_surface_t *s = find_surface(id);
    if (!s) return;
    s->w = w < s->min_w ? s->min_w : w;
    s->h = h < s->min_h ? s->min_h : h;
}

/* ── Snap / tile ── */

/* Multi-display awareness:
 * Displays are laid out horizontally (display 0 at x=0, then display 1
 * at x=screen_w0, etc.). For now, all queried displays share width
 * g_wm.screen_w (the WM's logical screen). This still gives us a
 * meaningful "which display does the window center live in?" answer
 * for kernels with one scanout (idx always 0), and a tile-able layout
 * for the multi-scanout case. */

static int wm_display_count_safe(void) {
    extern int gpu_virtio_display_count(void);
    int n = gpu_virtio_display_count();
    return n > 0 ? n : 1;
}

/* Returns origin x and width for a display index. */
static void wm_display_geom(int idx, int *out_x, int *out_w) {
    int dn = wm_display_count_safe();
    if (idx < 0)         idx = 0;
    if (idx >= dn)       idx = dn - 1;
    *out_x = idx * g_wm.screen_w;
    *out_w = g_wm.screen_w;
}

/* Which display contains the surface's center? */
static int wm_display_for_surface(chain_surface_t *s) {
    int cx = s->x + s->w / 2;
    int dn = wm_display_count_safe();
    int sw = g_wm.screen_w;
    int idx = (sw > 0) ? (cx / sw) : 0;
    if (idx < 0)   idx = 0;
    if (idx >= dn) idx = dn - 1;
    return idx;
}

/* Compute target geometry for a snap kind, scoped to the display the
 * surface is currently on. */
static void wm_compute_snap_target(chain_surface_t *s, snap_kind_t kind,
                                    int *tx, int *ty, int *tw, int *th)
{
    int disp = wm_display_for_surface(s);
    int dx, dw;
    wm_display_geom(disp, &dx, &dw);

    int top = g_wm.panel_h;
    int avail_h = g_wm.screen_h - top;
    int half_w = dw / 2;
    int half_h = avail_h / 2;
    int two_thirds_w = (dw * 2) / 3;

    *tx = s->x; *ty = s->y; *tw = s->w; *th = s->h;

    switch (kind) {
    case SURFACE_SNAPPED_LEFT:
        *tx = dx; *ty = top; *tw = half_w; *th = avail_h; break;
    case SURFACE_SNAPPED_RIGHT:
        *tx = dx + dw - half_w; *ty = top; *tw = half_w; *th = avail_h; break;
    case SURFACE_SNAPPED_LEFT_2_3:
        *tx = dx; *ty = top; *tw = two_thirds_w; *th = avail_h; break;
    case SURFACE_SNAPPED_RIGHT_2_3:
        *tx = dx + dw - two_thirds_w; *ty = top;
        *tw = two_thirds_w; *th = avail_h; break;
    case SURFACE_SNAPPED_TL:
        *tx = dx; *ty = top; *tw = half_w; *th = half_h; break;
    case SURFACE_SNAPPED_TR:
        *tx = dx + dw - half_w; *ty = top;
        *tw = half_w; *th = half_h; break;
    case SURFACE_SNAPPED_BL:
        *tx = dx; *ty = top + half_h; *tw = half_w; *th = half_h; break;
    case SURFACE_SNAPPED_BR:
        *tx = dx + dw - half_w; *ty = top + half_h;
        *tw = half_w; *th = half_h; break;
    case SURFACE_MAXIMIZED:
        *tx = dx; *ty = top; *tw = dw; *th = avail_h; break;
    default:
        /* Unsnap — restore saved */
        *tx = s->saved_x; *ty = s->saved_y;
        *tw = s->saved_w; *th = s->saved_h; break;
    }
}

void wm_snap_surface(int id, surface_state_t snap) {
    chain_surface_t *s = find_surface(id);
    if (!s) return;

    /* Save current geometry for unsnap (only on transition out of NORMAL) */
    if (s->state == SURFACE_NORMAL) {
        s->saved_x = s->x; s->saved_y = s->y;
        s->saved_w = s->w; s->saved_h = s->h;
    }

    int tx, ty, tw, th;
    snap_kind_t kind = snap;
    if (kind != SURFACE_NORMAL && kind != SURFACE_MAXIMIZED &&
        kind != SURFACE_SNAPPED_LEFT && kind != SURFACE_SNAPPED_RIGHT &&
        kind != SURFACE_SNAPPED_LEFT_2_3 && kind != SURFACE_SNAPPED_RIGHT_2_3 &&
        kind != SURFACE_SNAPPED_TL && kind != SURFACE_SNAPPED_TR &&
        kind != SURFACE_SNAPPED_BL && kind != SURFACE_SNAPPED_BR)
    {
        kind = SURFACE_NORMAL;
    }
    wm_compute_snap_target(s, kind, &tx, &ty, &tw, &th);

    /* Set final geometry (hit-testing uses these) */
    s->x = tx; s->y = ty; s->w = tw; s->h = th;

    float stiff, damp;
    snap_spring_constants(&stiff, &damp);
    spring_geom_to(s, tx, ty, tw, th, stiff, damp);

    s->state = kind;
}

/* Public canonical entry — same behavior, named per spec. */
void wm_snap(int window_id, snap_kind_t kind) {
    wm_snap_surface(window_id, kind);
}

/* Cycle: NORMAL -> LEFT half -> LEFT 2/3 -> NORMAL (restore). */
void wm_snap_cycle_left(int window_id) {
    chain_surface_t *s = find_surface(window_id);
    if (!s) return;
    snap_kind_t next;
    switch (s->state) {
    case SURFACE_SNAPPED_LEFT:        next = SURFACE_SNAPPED_LEFT_2_3; break;
    case SURFACE_SNAPPED_LEFT_2_3:    next = SURFACE_NORMAL;           break;
    default:                          next = SURFACE_SNAPPED_LEFT;     break;
    }
    wm_snap_surface(window_id, next);
}

void wm_snap_cycle_right(int window_id) {
    chain_surface_t *s = find_surface(window_id);
    if (!s) return;
    snap_kind_t next;
    switch (s->state) {
    case SURFACE_SNAPPED_RIGHT:       next = SURFACE_SNAPPED_RIGHT_2_3; break;
    case SURFACE_SNAPPED_RIGHT_2_3:   next = SURFACE_NORMAL;            break;
    default:                          next = SURFACE_SNAPPED_RIGHT;     break;
    }
    wm_snap_surface(window_id, next);
}

void wm_move_to_next_display(int window_id) {
    chain_surface_t *s = find_surface(window_id);
    if (!s) return;
    int dn = wm_display_count_safe();
    if (dn <= 1) return;  /* no other display */

    int cur = wm_display_for_surface(s);
    int nxt = (cur + 1) % dn;

    int cur_x, cur_w, nxt_x, nxt_w;
    wm_display_geom(cur, &cur_x, &cur_w);
    wm_display_geom(nxt, &nxt_x, &nxt_w);

    /* Translate window's x by the inter-display delta. Saved geometry
     * (for un-snap) follows so that restoring also lands on the new
     * display. */
    int dx = nxt_x - cur_x;
    int tx = s->x + dx;
    int ty = s->y;
    int tw = s->w;
    int th = s->h;

    s->saved_x += dx;
    s->x = tx; s->y = ty; s->w = tw; s->h = th;

    float stiff, damp;
    snap_spring_constants(&stiff, &damp);
    spring_geom_to(s, tx, ty, tw, th, stiff, damp);
}

int wm_cancel_drag(void) {
    int cancelled = 0;
    for (int i = 0; i < g_wm.surface_count; i++) {
        chain_surface_t *s = &g_wm.surfaces[i];
        if (s->dragging) {
            /* Restore window to drag-anchor position. */
            s->x = g_drag_anchor_x;
            s->y = g_drag_anchor_y;
            s->anim_x = (float)s->x;
            s->anim_y = (float)s->y;
            s->dragging = 0;
            cancelled = 1;
        }
    }
    g_wm.ghost.active = 0;
    return cancelled;
}

void wm_toggle_tiling(void) {
    int ws = g_wm.active_workspace;
    g_wm.tiling_enabled[ws] = !g_wm.tiling_enabled[ws];

    if (g_wm.tiling_enabled[ws]) {
        /* Auto-tile all visible surfaces on this workspace */
        int count = 0;
        for (int i = 0; i < g_wm.surface_count; i++) {
            chain_surface_t *s = &g_wm.surfaces[i];
            if (s->visible && s->workspace == ws) count++;
        }
        if (count == 0) return;

        int top = g_wm.panel_h;
        int avail_h = g_wm.screen_h - top;

        /* Simple grid: columns based on count */
        int cols = count <= 2 ? count : (count <= 4 ? 2 : 3);
        int rows = (count + cols - 1) / cols;
        int cell_w = g_wm.screen_w / cols;
        int cell_h = avail_h / rows;

        int idx = 0;
        for (int i = 0; i < g_wm.surface_count; i++) {
            chain_surface_t *s = &g_wm.surfaces[i];
            if (!s->visible || s->workspace != ws) continue;

            s->saved_x = s->x; s->saved_y = s->y;
            s->saved_w = s->w; s->saved_h = s->h;

            int col = idx % cols;
            int row = idx / cols;
            s->x = col * cell_w;
            s->y = top + row * cell_h;
            s->w = cell_w;
            s->h = cell_h;
            s->state = SURFACE_TILED;
            idx++;
        }
    } else {
        /* Restore all to saved positions */
        for (int i = 0; i < g_wm.surface_count; i++) {
            chain_surface_t *s = &g_wm.surfaces[i];
            if (s->workspace == g_wm.active_workspace && s->state == SURFACE_TILED) {
                s->x = s->saved_x; s->y = s->saved_y;
                s->w = s->saved_w; s->h = s->saved_h;
                s->state = SURFACE_NORMAL;
            }
        }
    }
}

/* ── Workspace ── */

void wm_switch_workspace(int workspace) {
    if (workspace < 0 || workspace >= WM_MAX_WORKSPACES) return;
    g_wm.active_workspace = workspace;
    g_wm.focused_id = -1;

    /* Focus topmost visible surface on new workspace */
    int best_z = -1;
    for (int i = 0; i < g_wm.surface_count; i++) {
        chain_surface_t *s = &g_wm.surfaces[i];
        if (s->visible && s->workspace == workspace && s->z_index > best_z) {
            best_z = s->z_index;
            g_wm.focused_id = s->id;
        }
    }
}

int wm_get_workspace(void) {
    return g_wm.active_workspace;
}

void wm_move_to_workspace(int id, int workspace) {
    /* Delegate to workspaces module so its window list and persistence
     * stay in sync. */
    workspace_move_window(id, workspace);
}

/* ── Input handling ── */

/* Hit test: which surface is under (x, y)? Returns ID or -1. */
static int hit_test(int x, int y) {
    int best_id = -1;
    int best_z = -1;

    for (int i = 0; i < g_wm.surface_count; i++) {
        chain_surface_t *s = &g_wm.surfaces[i];
        if (!s->visible || s->workspace != g_wm.active_workspace) continue;
        if (x >= s->x && x < s->x + s->w &&
            y >= s->y && y < s->y + s->h) {
            if (s->z_index > best_z) {
                best_z = s->z_index;
                best_id = s->id;
            }
        }
    }
    return best_id;
}

/* Check if click is on a window control button. Returns 0-3 or -1. */
static int hit_control(chain_surface_t *s, int x, int y) {
    if (y < s->y || y >= s->y + WM_TITLEBAR_HEIGHT) return -1;

    int btn_y = s->y + (WM_TITLEBAR_HEIGHT - WM_CONTROL_SIZE) / 2;
    if (y < btn_y || y >= btn_y + WM_CONTROL_SIZE) return -1;

    for (int b = 0; b < 4; b++) {
        int btn_x;
        if (g_wm.controls_side == WM_CONTROLS_LEFT) {
            btn_x = s->x + WM_CONTROL_MARGIN + b * (WM_CONTROL_SIZE + WM_CONTROL_SPACING);
        } else {
            btn_x = s->x + s->w - WM_CONTROL_MARGIN -
                    (4 - b) * (WM_CONTROL_SIZE + WM_CONTROL_SPACING);
        }
        if (x >= btn_x && x < btn_x + WM_CONTROL_SIZE)
            return b;
    }
    return -1;
}

/* Check if click is on a resize edge. Returns edge bitmask. */
static int hit_resize_edge(chain_surface_t *s, int x, int y) {
    int edge = 0;
    int bw = 4;  /* Resize grab area */

    if (y >= s->y + s->h - bw && y < s->y + s->h) edge |= 4;  /* bottom */
    if (x >= s->x + s->w - bw && x < s->x + s->w) edge |= 2;  /* right */
    if (y >= s->y && y < s->y + bw) edge |= 1;                  /* top */
    if (x >= s->x && x < s->x + bw) edge |= 8;                  /* left */

    return edge;
}

/* Detect snap zone from cursor position. Display-aware: zones are
 * relative to the display containing the cursor. */
static surface_state_t detect_snap_zone(int x, int y) {
    int top = g_wm.panel_h;
    int sh = g_wm.screen_h;
    int thr = g_snap_drag_threshold;
    int dn = wm_display_count_safe();

    /* Find display under cursor. */
    int sw = g_wm.screen_w;
    int disp = (sw > 0) ? (x / sw) : 0;
    if (disp < 0)   disp = 0;
    if (disp >= dn) disp = dn - 1;
    int dx, dw;
    wm_display_geom(disp, &dx, &dw);

    int local_x = x - dx;

    /* Corners first (higher priority) */
    if (local_x < thr && y < top + thr) return SURFACE_SNAPPED_TL;
    if (local_x >= dw - thr && y < top + thr) return SURFACE_SNAPPED_TR;
    if (local_x < thr && y >= sh - thr) return SURFACE_SNAPPED_BL;
    if (local_x >= dw - thr && y >= sh - thr) return SURFACE_SNAPPED_BR;

    /* Edges */
    if (local_x < thr) return SURFACE_SNAPPED_LEFT;
    if (local_x >= dw - thr) return SURFACE_SNAPPED_RIGHT;
    if (y < top + thr) return SURFACE_MAXIMIZED;

    return SURFACE_NORMAL;  /* No snap */
}

/* Compute ghost rect for a snap kind under cursor x/y. */
static void ghost_rect_for(int x, int y, surface_state_t kind,
                            int *gx, int *gy, int *gw, int *gh)
{
    int top = g_wm.panel_h;
    int avail_h = g_wm.screen_h - top;
    int dn = wm_display_count_safe();
    int sw = g_wm.screen_w;
    int disp = (sw > 0) ? (x / sw) : 0;
    if (disp < 0)   disp = 0;
    if (disp >= dn) disp = dn - 1;
    (void)y;

    int dx, dw;
    wm_display_geom(disp, &dx, &dw);
    int half_w = dw / 2;
    int half_h = avail_h / 2;

    *gx = dx; *gy = top; *gw = dw; *gh = avail_h;
    switch (kind) {
    case SURFACE_SNAPPED_LEFT:
        *gx = dx; *gy = top; *gw = half_w; *gh = avail_h; break;
    case SURFACE_SNAPPED_RIGHT:
        *gx = dx + dw - half_w; *gy = top;
        *gw = half_w; *gh = avail_h; break;
    case SURFACE_SNAPPED_TL:
        *gx = dx; *gy = top; *gw = half_w; *gh = half_h; break;
    case SURFACE_SNAPPED_TR:
        *gx = dx + dw - half_w; *gy = top;
        *gw = half_w; *gh = half_h; break;
    case SURFACE_SNAPPED_BL:
        *gx = dx; *gy = top + half_h;
        *gw = half_w; *gh = half_h; break;
    case SURFACE_SNAPPED_BR:
        *gx = dx + dw - half_w; *gy = top + half_h;
        *gw = half_w; *gh = half_h; break;
    case SURFACE_MAXIMIZED:
        *gx = dx; *gy = top; *gw = dw; *gh = avail_h; break;
    default: break;
    }
}

void wm_mouse_down(int x, int y, int button) {
    (void)button;

    int id = hit_test(x, y);
    if (id < 0) return;

    wm_focus_surface(id);
    chain_surface_t *s = find_surface(id);
    if (!s) return;

    /* Check window control buttons */
    int ctrl = hit_control(s, x, y);
    if (ctrl >= 0) {
        switch (ctrl) {
        case 0: wm_detach_surface(id); break;     /* [×] detach */
        case 1: wm_minimize_surface(id); break;    /* [−] minimize */
        case 2: wm_maximize_surface(id); break;    /* [□] maximize */
        case 3: /* [⚡] signal status — toggle pause? */ break;
        }
        return;
    }

    /* Check resize edges */
    int edge = hit_resize_edge(s, x, y);
    if (edge) {
        s->resizing = 1;
        s->resize_edge = edge;
        return;
    }

    /* Title bar drag */
    if (y >= s->y && y < s->y + WM_TITLEBAR_HEIGHT) {
        /* Double-click on titlebar = maximize toggle */
        s->dragging = 1;
        s->drag_offset_x = x - s->x;
        s->drag_offset_y = y - s->y;
        /* Anchor for Escape-cancel. */
        g_drag_anchor_x = s->x;
        g_drag_anchor_y = s->y;

        /* Unsnap if dragging a snapped window — spring resize back */
        if (s->state != SURFACE_NORMAL && s->state != SURFACE_TILED) {
            int old_w = s->w, old_h = s->h;
            s->w = s->saved_w;
            s->h = s->saved_h;
            s->state = SURFACE_NORMAL;

            /* Spring-animate the size change (position follows mouse) */
            s->anim_w = (float)old_w;
            s->anim_h = (float)old_h;
            cancel_geom_anims(s);
            s->anim_w_id = anim_spring((float)old_w, (float)s->w,
                                        SPRING_SNAPPY_S, SPRING_SNAPPY_D,
                                        surface_w_cb, s);
            s->anim_h_id = anim_spring((float)old_h, (float)s->h,
                                        SPRING_SNAPPY_S, SPRING_SNAPPY_D,
                                        surface_h_cb, s);
        }
    }
}

void wm_mouse_up(int x, int y, int button) {
    (void)button;

    for (int i = 0; i < g_wm.surface_count; i++) {
        chain_surface_t *s = &g_wm.surfaces[i];

        if (s->dragging) {
            s->dragging = 0;

            /* Check if we should snap — spring-animate to snap target */
            surface_state_t snap = detect_snap_zone(x, y);
            if (snap != SURFACE_NORMAL) {
                /* Sync anim position to current drag position before snapping */
                s->anim_x = (float)s->x;
                s->anim_y = (float)s->y;
                s->anim_w = (float)s->w;
                s->anim_h = (float)s->h;
                wm_snap_surface(s->id, snap);
            }
        }

        if (s->resizing) {
            s->resizing = 0;
        }
    }

    g_wm.ghost.active = 0;
}

void wm_mouse_move(int x, int y) {
    for (int i = 0; i < g_wm.surface_count; i++) {
        chain_surface_t *s = &g_wm.surfaces[i];

        if (s->dragging) {
            s->x = x - s->drag_offset_x;
            s->y = y - s->drag_offset_y;
            /* Keep animated position in sync during drag */
            s->anim_x = (float)s->x;
            s->anim_y = (float)s->y;

            /* Update snap ghost (display-aware via ghost_rect_for). */
            surface_state_t snap = detect_snap_zone(x, y);
            if (snap != SURFACE_NORMAL) {
                g_wm.ghost.active = 1;
                g_wm.ghost.target_state = snap;
                ghost_rect_for(x, y, snap,
                               &g_wm.ghost.x, &g_wm.ghost.y,
                               &g_wm.ghost.w, &g_wm.ghost.h);
            } else {
                g_wm.ghost.active = 0;
            }
        }

        if (s->resizing) {
            if (s->resize_edge & 2) /* right */
                s->w = x - s->x;
            if (s->resize_edge & 4) /* bottom */
                s->h = y - s->y;
            if (s->resize_edge & 8) { /* left */
                int right = s->x + s->w;
                s->x = x;
                s->w = right - x;
            }
            if (s->resize_edge & 1) { /* top */
                int bottom = s->y + s->h;
                s->y = y;
                s->h = bottom - y;
            }
            if (s->w < s->min_w) s->w = s->min_w;
            if (s->h < s->min_h) s->h = s->min_h;
        }
    }
}

/* ── Rendering ── */

void wm_draw_chrome(chain_surface_t *s) {
    uint32_t title_bg = s->focused ? COLOR_SURFACE_HIGH : COLOR_SURFACE;
    uint32_t border = s->focused ? s->accent : COLOR_SEPARATOR;

    /* Border */
    fb_rect_outline(s->x, s->y, s->w, s->h, border, 1);

    /* Title bar background */
    fb_rect(s->x + 1, s->y + 1, s->w - 2, WM_TITLEBAR_HEIGHT - 1, title_bg);

    /* Separator under title bar */
    fb_hline(s->x, s->y + WM_TITLEBAR_HEIGHT, s->w, COLOR_SEPARATOR);

    /* Window control buttons */
    uint32_t ctrl_colors[4] = {
        COLOR_DANGER,   /* [×] detach — red */
        COLOR_WARNING,  /* [−] minimize — amber */
        COLOR_SUCCESS,  /* [□] maximize — green */
        s->signal == SIGNAL_LIVE ? COLOR_SUCCESS :
        s->signal == SIGNAL_ERROR ? COLOR_DANGER : COLOR_ON_SURFACE_4
    };

    char ctrl_chars[4] = { 'x', '-', '+', '!' };

    for (int b = 0; b < 4; b++) {
        int btn_x, btn_y;
        btn_y = s->y + (WM_TITLEBAR_HEIGHT - WM_CONTROL_SIZE) / 2;

        if (g_wm.controls_side == WM_CONTROLS_LEFT) {
            btn_x = s->x + WM_CONTROL_MARGIN +
                    b * (WM_CONTROL_SIZE + WM_CONTROL_SPACING);
        } else {
            btn_x = s->x + s->w - WM_CONTROL_MARGIN -
                    (4 - b) * (WM_CONTROL_SIZE + WM_CONTROL_SPACING);
        }

        /* Button circle */
        fb_circle_filled(btn_x + WM_CONTROL_SIZE / 2,
                        btn_y + WM_CONTROL_SIZE / 2,
                        WM_CONTROL_SIZE / 2 - 1,
                        ctrl_colors[b]);

        /* Button label (tiny) */
        char label[2] = { ctrl_chars[b], 0 };
        fb_text(btn_x + 4, btn_y + 2, label, COLOR_SURFACE);
    }

    /* Title text */
    int title_x;
    if (g_wm.controls_side == WM_CONTROLS_LEFT) {
        title_x = s->x + WM_CONTROL_MARGIN +
                  4 * (WM_CONTROL_SIZE + WM_CONTROL_SPACING) + 8;
    } else {
        title_x = s->x + WM_CONTROL_MARGIN + 8;
    }

    font_draw(title_x, s->y + 8, s->title, FONT_UI, TYPE_LABEL,
              s->focused ? COLOR_ON_SURFACE : COLOR_ON_SURFACE_2);
}

void wm_draw_ghost(void) {
    if (!g_wm.ghost.active) return;

    /* Translucent accent rectangle */
    uint32_t ghost_color = (COLOR_PRIMARY & 0x00FFFFFF) | 0x30000000;  /* ~19% opacity */
    fb_rect_blend(g_wm.ghost.x, g_wm.ghost.y,
                  g_wm.ghost.w, g_wm.ghost.h, ghost_color);

    /* Outline */
    fb_rect_outline(g_wm.ghost.x, g_wm.ghost.y,
                    g_wm.ghost.w, g_wm.ghost.h, COLOR_PRIMARY, 1);
}

/* Sort surfaces by z-index for draw order */
static void sort_by_z(int *indices, int count) {
    /* Simple insertion sort — max 32 surfaces */
    for (int i = 1; i < count; i++) {
        int key = indices[i];
        int j = i - 1;
        while (j >= 0 && g_wm.surfaces[indices[j]].z_index > g_wm.surfaces[key].z_index) {
            indices[j + 1] = indices[j];
            j--;
        }
        indices[j + 1] = key;
    }
}

void wm_draw_all(void) {
    /* Collect visible surfaces on current workspace and (during a
     * workspace slide) on the incoming workspace. */
    int visible[WM_MAX_SURFACES];
    int vis_count = 0;
    int sliding = workspaces_is_sliding();
    int incoming = workspaces_slide_incoming_id();
    int active_off = workspaces_slide_offset_active();
    int incoming_off = workspaces_slide_offset_incoming();

    for (int i = 0; i < g_wm.surface_count; i++) {
        chain_surface_t *s = &g_wm.surfaces[i];
        if (!(s->visible || s->closing)) continue;
        if (s->workspace == g_wm.active_workspace ||
            (sliding && s->workspace == incoming))
            visible[vis_count++] = i;
    }

    /* Sort by z-index (lowest first = painted first) */
    sort_by_z(visible, vis_count);

    /* NOTE: Snap ghost is drawn by the compositor between notify_draw
     * and context_menu_draw so it sits above windows during drag. */

    /* Draw each surface (with spring-animated scale, opacity, geometry) */
    for (int v = 0; v < vis_count; v++) {
        chain_surface_t *s = &g_wm.surfaces[visible[v]];

        /* Skip fully transparent surfaces */
        if (s->anim_opacity < 1.0f) continue;

        /* Compute rendered geometry from animated values.
         * Geometry springs (anim_x/y/w/h) are used for snap/maximize/show-desktop.
         * If no geometry anim is running, use the logical x/y/w/h directly.
         */
        int rx, ry, rw, rh;
        int geom_animating = (s->anim_x_id >= 0 && anim_is_active(s->anim_x_id)) ||
                             (s->anim_y_id >= 0 && anim_is_active(s->anim_y_id)) ||
                             (s->anim_w_id >= 0 && anim_is_active(s->anim_w_id)) ||
                             (s->anim_h_id >= 0 && anim_is_active(s->anim_h_id));

        if (geom_animating) {
            rx = (int)s->anim_x;
            ry = (int)s->anim_y;
            rw = (int)s->anim_w;
            rh = (int)s->anim_h;
        } else {
            rx = s->x;
            ry = s->y;
            rw = s->w;
            rh = s->h;
            /* Sync anim state to logical state when not animating */
            s->anim_x = (float)s->x;
            s->anim_y = (float)s->y;
            s->anim_w = (float)s->w;
            s->anim_h = (float)s->h;
        }

        /* Workspace slide offset — apply to whichever side this surface
         * belongs to. */
        if (sliding) {
            if (s->workspace == g_wm.active_workspace)
                rx += active_off;
            else if (s->workspace == incoming)
                rx += incoming_off;
        }

        /* Apply scale: shrink around center */
        float scale = s->anim_scale;
        if (scale < 0.01f) scale = 0.01f;
        if (scale < 1.0f) {
            int cxc = rx + rw / 2;
            int cyc = ry + rh / 2;
            rw = (int)((float)rw * scale);
            rh = (int)((float)rh * scale);
            rx = cxc - rw / 2;
            ry = cyc - rh / 2;
        }

        /* Compute opacity alpha (0-255) */
        int alpha = (int)s->anim_opacity;
        if (alpha < 0) alpha = 0;
        if (alpha > 255) alpha = 255;

        /* Shadow (focused windows get stronger shadow) */
        shadow_t shadow = s->focused ? SHADOW_L2 : SHADOW_L1;
        int so = (int)shadow.opacity * alpha / 255;
        uint32_t shadow_color = (uint32_t)so << 24;
        fb_rect_blend(rx + shadow.offset_y, ry + shadow.offset_y,
                      rw, rh, shadow_color);

        /* Chrome — use rendered geometry */
        /* Temporarily swap geometry for chrome drawing */
        int ox = s->x, oy = s->y, ow = s->w, oh = s->h;
        s->x = rx; s->y = ry; s->w = rw; s->h = rh;
        wm_draw_chrome(s);
        s->x = ox; s->y = oy; s->w = ow; s->h = oh;

        /* Content area background */
        int cx = rx + 1;
        int cy = ry + WM_TITLEBAR_HEIGHT + 1;
        int cw = rw - 2;
        int ch = rh - WM_TITLEBAR_HEIGHT - 2;

        if (alpha >= 255) {
            fb_rect(cx, cy, cw, ch, COLOR_SURFACE);
        } else {
            /* Blend content background with opacity */
            uint32_t bg = (COLOR_SURFACE & 0x00FFFFFF) | ((uint32_t)alpha << 24);
            fb_rect_blend(cx, cy, cw, ch, bg);
        }

        /* Content callback */
        if (s->draw_content)
            s->draw_content(s->id, cx, cy, cw, ch);
    }
}

/* ── Show Desktop ── */

void wm_show_desktop(void) {
    g_desktop_shown = 1;

    for (int i = 0; i < g_wm.surface_count; i++) {
        chain_surface_t *s = &g_wm.surfaces[i];
        if (!s->visible || s->workspace != g_wm.active_workspace) continue;

        /* Scale to 0.9, opacity to 0 */
        if (s->anim_scale_id >= 0 && anim_is_active(s->anim_scale_id))
            anim_retarget(s->anim_scale_id, 0.9f);
        else
            s->anim_scale_id = anim_spring(s->anim_scale, 0.9f,
                                            SPRING_SMOOTH_S, SPRING_SMOOTH_D,
                                            surface_scale_cb, s);

        if (s->anim_opacity_id >= 0 && anim_is_active(s->anim_opacity_id))
            anim_retarget(s->anim_opacity_id, 0.0f);
        else
            s->anim_opacity_id = anim_spring(s->anim_opacity, 0.0f,
                                              SPRING_SMOOTH_S, SPRING_SMOOTH_D,
                                              surface_opacity_cb, s);
    }
}

void wm_restore_desktop(void) {
    g_desktop_shown = 0;

    for (int i = 0; i < g_wm.surface_count; i++) {
        chain_surface_t *s = &g_wm.surfaces[i];
        if (!s->visible || s->workspace != g_wm.active_workspace) continue;

        /* Scale back to 1.0, opacity back to 255 */
        if (s->anim_scale_id >= 0 && anim_is_active(s->anim_scale_id))
            anim_retarget(s->anim_scale_id, 1.0f);
        else
            s->anim_scale_id = anim_spring(s->anim_scale, 1.0f,
                                            SPRING_SMOOTH_S, SPRING_SMOOTH_D,
                                            surface_scale_cb, s);

        if (s->anim_opacity_id >= 0 && anim_is_active(s->anim_opacity_id))
            anim_retarget(s->anim_opacity_id, 255.0f);
        else
            s->anim_opacity_id = anim_spring(s->anim_opacity, 255.0f,
                                              SPRING_SMOOTH_S, SPRING_SMOOTH_D,
                                              surface_opacity_cb, s);
    }
}

int wm_is_desktop_shown(void) {
    return g_desktop_shown;
}

/* ── Queries ── */

chain_surface_t *wm_get_surface(int id) {
    return find_surface(id);
}

chain_surface_t *wm_get_surface_by_index(int index) {
    if (index < 0 || index >= g_wm.surface_count) return 0;
    return &g_wm.surfaces[index];
}

int wm_surface_count(void) {
    return g_wm.surface_count;
}

int wm_visible_count(void) {
    int count = 0;
    for (int i = 0; i < g_wm.surface_count; i++) {
        chain_surface_t *s = &g_wm.surfaces[i];
        if (s->visible && s->workspace == g_wm.active_workspace)
            count++;
    }
    return count;
}
