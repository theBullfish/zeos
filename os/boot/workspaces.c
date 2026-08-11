/*
 * Virtual desktops. N workspaces (default 4, max 8). Per-workspace
 * window list, wallpaper, dock running-app filter. Super+Ctrl+arrow
 * switch, Super+Ctrl+Up overview, Super+Ctrl+Shift+N move-and-switch.
 * Persists to VAULT.
 */

#include "workspaces.h"
#include "wm.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "anim.h"
#include "vault.h"
#include "kprint.h"
#include "compositor.h"

/* ── State ── */

static workspace_t g_ws[WS_MAX_COUNT];
static int g_ws_count = WS_DEFAULT_COUNT;
static int g_ws_active = 0;
static int g_anim_ms = 200;

static int g_screen_w = 0;
static int g_screen_h = 0;

/* Slide animation: a single horizontal offset spring, driving both
 * outgoing (active) and incoming workspaces. While the spring is alive
 * we render two workspaces side-by-side, separated by g_screen_w. */
static int   g_sliding = 0;
static int   g_slide_incoming_id = -1;
static int   g_slide_dir = 0;            /* +1 = next, -1 = prev */
static float g_slide_pos = 0.0f;         /* 0..1 progress */
static int   g_slide_anim_id = -1;

/* Overview mode */
static int g_overview = 0;
static int g_overview_hover_ws = -1;

/* ── Helpers ── */

static void str_copy(char *d, const char *s, int max) {
    int i = 0;
    while (s && s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void str_default_ws_name(char *out, int id) {
    /* "Workspace N" */
    const char *p = "Workspace ";
    int o = 0;
    while (*p && o < WS_NAME_MAX - 3) out[o++] = *p++;
    int n = id + 1;
    if (n >= 10) {
        out[o++] = '0' + (n / 10);
        out[o++] = '0' + (n % 10);
    } else {
        out[o++] = '0' + n;
    }
    out[o] = 0;
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Sort window list of a workspace by current z_index. */
static void ws_sort_by_z(workspace_t *w) {
    for (int i = 1; i < w->window_count; i++) {
        int key = w->window_ids[i];
        chain_surface_t *ks = wm_get_surface(key);
        int kz = ks ? ks->z_index : 0;
        int j = i - 1;
        while (j >= 0) {
            chain_surface_t *js = wm_get_surface(w->window_ids[j]);
            int jz = js ? js->z_index : 0;
            if (jz <= kz) break;
            w->window_ids[j + 1] = w->window_ids[j];
            j--;
        }
        w->window_ids[j + 1] = key;
    }
}

/* Rebuild every workspace's window list from WM truth. */
static void workspaces_rebuild_lists(void) {
    for (int i = 0; i < WS_MAX_COUNT; i++)
        g_ws[i].window_count = 0;

    int n = wm_surface_count();
    for (int i = 0; i < n; i++) {
        chain_surface_t *s = wm_get_surface_by_index(i);
        if (!s) continue;
        int wid = s->workspace;
        if (wid < 0 || wid >= g_ws_count) {
            wid = 0;
            s->workspace = 0;
        }
        workspace_t *w = &g_ws[wid];
        if (w->window_count < WS_MAX_WINDOWS)
            w->window_ids[w->window_count++] = s->id;
    }

    for (int i = 0; i < g_ws_count; i++)
        ws_sort_by_z(&g_ws[i]);
}

/* ── Init ── */

void workspaces_init(int screen_w, int screen_h) {
    g_screen_w = screen_w;
    g_screen_h = screen_h;

    g_ws_count = WS_DEFAULT_COUNT;
    g_ws_active = 0;
    g_sliding = 0;
    g_slide_incoming_id = -1;
    g_slide_anim_id = -1;
    g_slide_pos = 0.0f;
    g_overview = 0;
    g_overview_hover_ws = -1;

    for (int i = 0; i < WS_MAX_COUNT; i++) {
        g_ws[i].id = i;
        str_default_ws_name(g_ws[i].name, i);
        g_ws[i].wallpaper_path[0] = 0;
        g_ws[i].window_count = 0;
        g_ws[i].focused_window_id = -1;
    }

    workspaces_load();

    kputs("WS: initialized count=");
    kput_dec((uint64_t)g_ws_count);
    kputs(" active=");
    kput_dec((uint64_t)g_ws_active);
    kputs("\n");
}

void workspaces_set_count(int n) {
    n = clamp_int(n, 1, WS_MAX_COUNT);
    if (n == g_ws_count) return;

    /* If shrinking, re-home any window on a now-out-of-range workspace */
    if (n < g_ws_count) {
        int wm_n = wm_surface_count();
        for (int i = 0; i < wm_n; i++) {
            chain_surface_t *s = wm_get_surface_by_index(i);
            if (!s) continue;
            if (s->workspace >= n) s->workspace = 0;
        }
        if (g_ws_active >= n) g_ws_active = 0;
    }

    g_ws_count = n;
    workspaces_rebuild_lists();
    workspaces_save();
    compositor_dirty_all();
}

int workspaces_get_count(void) { return g_ws_count; }

void workspaces_set_animation_ms(int ms) {
    g_anim_ms = clamp_int(ms, 30, 2000);
}
int workspaces_get_animation_ms(void) { return g_anim_ms; }

int workspace_active(void) { return g_ws_active; }

const workspace_t *workspace_get(int id) {
    if (id < 0 || id >= g_ws_count) return 0;
    return &g_ws[id];
}
workspace_t *workspace_get_mut(int id) {
    if (id < 0 || id >= g_ws_count) return 0;
    return &g_ws[id];
}

/* ── Slide animation ── */

static void slide_cb(int aid, float pos, void *ctx) {
    (void)ctx;
    g_slide_pos = pos;
    /* Final callback fires after the anim is marked inactive. */
    if (!anim_is_active(aid)) {
        /* Finalize: incoming becomes active. */
        if (g_slide_incoming_id >= 0) {
            g_ws_active = g_slide_incoming_id;
            wm_switch_workspace(g_ws_active);

            /* Restore focused window for that workspace */
            workspace_t *w = &g_ws[g_ws_active];
            if (w->focused_window_id >= 0) {
                chain_surface_t *fs = wm_get_surface(w->focused_window_id);
                if (fs && fs->workspace == g_ws_active && fs->visible)
                    wm_focus_surface(w->focused_window_id);
            }
        }
        g_sliding = 0;
        g_slide_pos = 0.0f;
        g_slide_incoming_id = -1;
        g_slide_dir = 0;
        compositor_dirty_all();
        workspaces_save();
    }
    compositor_dirty_all();
}

/* Spring constants from animation_ms. Shorter ms = stiffer.
 * Mapped onto theme.h presets where possible:
 *  - <=140ms : extra-snappy derivation (no preset; tightest user-tunable)
 *  - <=260ms : INTERACTIVE preset (default snap window)
 *  - else    : SNAPPY preset (relaxed)
 */
static void slide_spring_constants(float *stiff, float *damp) {
    if (g_anim_ms <= 140) {
        *stiff = 600.0f; *damp = 32.0f;
    } else if (g_anim_ms <= 260) {
        *stiff = SPRING_INTERACTIVE_S; *damp = SPRING_INTERACTIVE_D;
    } else {
        *stiff = SPRING_SNAPPY_S; *damp = SPRING_SNAPPY_D;
    }
}

void workspace_switch(int target_id) {
    if (target_id < 0 || target_id >= g_ws_count) return;
    if (target_id == g_ws_active && !g_sliding) return;
    if (g_overview) workspaces_overview_exit();

    /* Save current focus into outgoing workspace */
    {
        workspace_t *cur = &g_ws[g_ws_active];
        cur->focused_window_id = wm_get_focused();
    }

    /* If a slide is already in flight, retarget atomically. */
    if (g_sliding && g_slide_anim_id >= 0 && anim_is_active(g_slide_anim_id)) {
        anim_cancel(g_slide_anim_id);
        g_slide_anim_id = -1;
    }

    g_slide_incoming_id = target_id;
    g_slide_dir = (target_id > g_ws_active) ? +1 : -1;
    g_sliding = 1;
    g_slide_pos = 0.0f;

    float stiff, damp;
    slide_spring_constants(&stiff, &damp);
    g_slide_anim_id = anim_spring(0.0f, 1.0f, stiff, damp, slide_cb, 0);

    compositor_dirty_all();

    kputs("WS: switch ");
    kput_dec((uint64_t)g_ws_active);
    kputs(" -> ");
    kput_dec((uint64_t)target_id);
    kputs("\n");
}

void workspace_move_window(int window_id, int target_id) {
    if (target_id < 0 || target_id >= g_ws_count) return;
    chain_surface_t *s = wm_get_surface(window_id);
    if (!s) return;
    int prev = s->workspace;
    if (prev == target_id) return;

    s->workspace = target_id;

    /* Update lists */
    workspace_t *src = &g_ws[prev];
    for (int i = 0; i < src->window_count; i++) {
        if (src->window_ids[i] == window_id) {
            for (int j = i; j < src->window_count - 1; j++)
                src->window_ids[j] = src->window_ids[j + 1];
            src->window_count--;
            break;
        }
    }
    if (src->focused_window_id == window_id)
        src->focused_window_id = (src->window_count > 0)
            ? src->window_ids[src->window_count - 1] : -1;

    workspace_t *dst = &g_ws[target_id];
    if (dst->window_count < WS_MAX_WINDOWS)
        dst->window_ids[dst->window_count++] = window_id;

    workspaces_save();
    compositor_dirty_all();
}

void workspace_move_focused_and_switch(int target_id) {
    int f = wm_get_focused();
    if (f >= 0) workspace_move_window(f, target_id);
    workspace_switch(target_id);
}

/* ── Wallpaper ── */

void workspace_set_wallpaper(int id, const char *path) {
    if (id < 0 || id >= WS_MAX_COUNT) return;
    str_copy(g_ws[id].wallpaper_path, path ? path : "", WS_WALLPAPER_MAX);
    workspaces_save();
    compositor_dirty_all();
}

const char *workspace_get_wallpaper(int id) {
    if (id < 0 || id >= WS_MAX_COUNT) return "";
    return g_ws[id].wallpaper_path;
}

/* ── Window assignment hooks ── */

void workspaces_window_assigned(int window_id, int workspace_id) {
    if (workspace_id < 0 || workspace_id >= g_ws_count) return;
    workspace_t *w = &g_ws[workspace_id];
    for (int i = 0; i < w->window_count; i++)
        if (w->window_ids[i] == window_id) return;
    if (w->window_count < WS_MAX_WINDOWS)
        w->window_ids[w->window_count++] = window_id;
}

void workspaces_window_removed(int window_id) {
    for (int i = 0; i < g_ws_count; i++) {
        workspace_t *w = &g_ws[i];
        for (int j = 0; j < w->window_count; j++) {
            if (w->window_ids[j] == window_id) {
                for (int k = j; k < w->window_count - 1; k++)
                    w->window_ids[k] = w->window_ids[k + 1];
                w->window_count--;
                if (w->focused_window_id == window_id)
                    w->focused_window_id = -1;
                break;
            }
        }
    }
}

void workspaces_window_focused(int window_id) {
    chain_surface_t *s = wm_get_surface(window_id);
    if (!s) return;
    int wid = s->workspace;
    if (wid < 0 || wid >= g_ws_count) return;
    g_ws[wid].focused_window_id = window_id;
}

/* ── Slide-offset queries used by WM at draw time ── */

int workspaces_is_sliding(void) { return g_sliding; }
int workspaces_slide_incoming_id(void) {
    return g_sliding ? g_slide_incoming_id : -1;
}

/* Pixel offset applied to active workspace windows during slide.
 * dir +1 (going forward): active slides left  -> offset = -p*W
 * dir -1 (going back):    active slides right -> offset = +p*W */
int workspaces_slide_offset_active(void) {
    if (!g_sliding) return 0;
    int w = g_screen_w > 0 ? g_screen_w : (int)fb_width();
    float p = g_slide_pos;
    if (p < 0) p = 0;
    if (p > 1) p = 1;
    return (int)((float)(-g_slide_dir * w) * p);
}

/* Incoming workspace starts off-screen on the opposite side. */
int workspaces_slide_offset_incoming(void) {
    if (!g_sliding) return 0;
    int w = g_screen_w > 0 ? g_screen_w : (int)fb_width();
    float p = g_slide_pos;
    if (p < 0) p = 0;
    if (p > 1) p = 1;
    /* dir +1: incoming starts at +W, ends at 0 -> offset = (1-p)*W
     * dir -1: incoming starts at -W, ends at 0 -> offset = -(1-p)*W */
    return (int)((float)(g_slide_dir * w) * (1.0f - p));
}

/* ── Overview ── */

void workspaces_overview_enter(void) {
    if (g_overview) return;
    g_overview = 1;
    g_overview_hover_ws = g_ws_active;
    workspaces_rebuild_lists();
    compositor_dirty_all();
    kputs("WS: overview enter\n");
}

void workspaces_overview_exit(void) {
    if (!g_overview) return;
    g_overview = 0;
    g_overview_hover_ws = -1;
    compositor_dirty_all();
    kputs("WS: overview exit\n");
}

int workspaces_overview_active(void) { return g_overview; }

int workspaces_overview_handle_escape(void) {
    if (!g_overview) return 0;
    workspaces_overview_exit();
    return 1;
}

/* Compute layout: cols x rows grid that holds g_ws_count tiles. */
static void overview_grid(int *cols, int *rows) {
    int n = g_ws_count;
    if (n <= 1) { *cols = 1; *rows = 1; return; }
    if (n <= 2) { *cols = 2; *rows = 1; return; }
    if (n <= 4) { *cols = 2; *rows = 2; return; }
    if (n <= 6) { *cols = 3; *rows = 2; return; }
    *cols = 4; *rows = 2;
}

static void overview_tile_rect(int idx, int *out_x, int *out_y,
                               int *out_w, int *out_h) {
    int sw = g_screen_w > 0 ? g_screen_w : (int)fb_width();
    int sh = g_screen_h > 0 ? g_screen_h : (int)fb_height();
    int cols, rows;
    overview_grid(&cols, &rows);

    int margin_x = 32;
    int margin_y = 48;
    int gap = 24;
    int avail_w = sw - margin_x * 2 - gap * (cols - 1);
    int avail_h = sh - margin_y * 2 - gap * (rows - 1);
    int tw = avail_w / cols;
    int th = avail_h / rows;

    int col = idx % cols;
    int row = idx / cols;
    *out_x = margin_x + col * (tw + gap);
    *out_y = margin_y + row * (th + gap);
    *out_w = tw;
    *out_h = th;
}

/* Map a real-screen rectangle into a tile's miniature rectangle. */
static void overview_map_rect(int idx, int rx, int ry, int rw, int rh,
                              int *mx, int *my, int *mw, int *mh) {
    int tx, ty, tw, th;
    overview_tile_rect(idx, &tx, &ty, &tw, &th);
    int sw = g_screen_w > 0 ? g_screen_w : (int)fb_width();
    int sh = g_screen_h > 0 ? g_screen_h : (int)fb_height();
    if (sw <= 0) sw = 1;
    if (sh <= 0) sh = 1;
    *mx = tx + (rx * tw) / sw;
    *my = ty + (ry * th) / sh;
    *mw = (rw * tw) / sw;
    *mh = (rh * th) / sh;
    if (*mw < 2) *mw = 2;
    if (*mh < 2) *mh = 2;
}

void workspaces_overview_draw(void) {
    if (!g_overview) return;

    int sw = g_screen_w > 0 ? g_screen_w : (int)fb_width();
    int sh = g_screen_h > 0 ? g_screen_h : (int)fb_height();

    /* Dim background */
    fb_rect_blend(0, 0, sw, sh, 0xC0000000);

    /* Refresh stack ordering for each workspace */
    workspaces_rebuild_lists();

    for (int i = 0; i < g_ws_count; i++) {
        int tx, ty, tw, th;
        overview_tile_rect(i, &tx, &ty, &tw, &th);

        /* Tile background */
        uint32_t tile_bg = COLOR_SURFACE;
        if (g_ws[i].wallpaper_path[0] == 0) {
            tile_bg = COLOR_SURFACE_HIGH;
        }
        fb_rect(tx, ty, tw, th, tile_bg);

        /* Border: accent for active, hover, or default */
        uint32_t border = COLOR_SEPARATOR;
        int thickness = 1;
        if (i == g_ws_active) {
            border = COLOR_PRIMARY;
            thickness = 2;
        }
        if (i == g_overview_hover_ws) {
            border = COLOR_PRIMARY;
            thickness = 3;
        }
        fb_rect_outline(tx, ty, tw, th, border, thickness);

        /* Miniature window list */
        workspace_t *w = &g_ws[i];
        for (int j = 0; j < w->window_count; j++) {
            chain_surface_t *s = wm_get_surface(w->window_ids[j]);
            if (!s || (!s->visible && s->state != SURFACE_MINIMIZED)) continue;

            int mx, my, mw, mh;
            overview_map_rect(i, s->x, s->y, s->w, s->h, &mx, &my, &mw, &mh);
            /* Clamp inside the tile */
            if (mx < tx) { mw -= (tx - mx); mx = tx; }
            if (my < ty) { mh -= (ty - my); my = ty; }
            if (mx + mw > tx + tw) mw = tx + tw - mx;
            if (my + mh > ty + th) mh = ty + th - my;
            if (mw <= 0 || mh <= 0) continue;

            uint32_t mini_bg = (s->accent & 0x00FFFFFF) | 0x80000000;
            fb_rect_blend(mx, my, mw, mh, mini_bg);
            fb_rect_outline(mx, my, mw, mh, s->accent, 1);
        }

        /* Label below tile */
        int label_y = ty + th + 4;
        if (label_y + 14 < sh)
            fb_text(tx + 6, label_y, w->name, COLOR_ON_SURFACE);
    }

    /* Hint text bottom-center */
    const char *hint = "Esc to exit";
    fb_text(sw / 2 - 32, sh - 20, hint, COLOR_ON_SURFACE_2);
}

/* Hit-test which tile is at (x, y). -1 if none. */
static int overview_tile_at(int x, int y) {
    for (int i = 0; i < g_ws_count; i++) {
        int tx, ty, tw, th;
        overview_tile_rect(i, &tx, &ty, &tw, &th);
        if (x >= tx && x < tx + tw && y >= ty && y < ty + th)
            return i;
    }
    return -1;
}

int workspaces_overview_mouse_down(int x, int y, int button) {
    if (!g_overview) return 0;
    if (button != 1) {
        /* Right-click in overview also exits. */
        workspaces_overview_exit();
        return 1;
    }
    int idx = overview_tile_at(x, y);
    if (idx >= 0) {
        workspaces_overview_exit();
        if (idx != g_ws_active) workspace_switch(idx);
        return 1;
    }
    /* Click in empty overview area = exit */
    workspaces_overview_exit();
    return 1;
}

int workspaces_overview_mouse_move(int x, int y) {
    if (!g_overview) return 0;
    int idx = overview_tile_at(x, y);
    if (idx != g_overview_hover_ws) {
        g_overview_hover_ws = idx;
        compositor_dirty_all();
    }
    return 1;
}

/* ── Indicator (panel.c) ── */

#define WS_DOT_RADIUS  3
#define WS_DOT_PAD     6

static int dot_strip_width(void) {
    return g_ws_count * (WS_DOT_RADIUS * 2) + (g_ws_count - 1) * WS_DOT_PAD;
}

int workspaces_draw_indicator(int x, int y_center) {
    int total_w = dot_strip_width();
    int cx = x + WS_DOT_RADIUS;
    for (int i = 0; i < g_ws_count; i++) {
        uint32_t color;
        if (i == g_ws_active)
            color = COLOR_PRIMARY;
        else
            color = COLOR_ON_SURFACE_4;
        fb_circle_filled(cx, y_center, WS_DOT_RADIUS, color);
        cx += WS_DOT_RADIUS * 2 + WS_DOT_PAD;
    }
    return total_w;
}

int workspaces_indicator_hit(int x, int y, int strip_x, int y_center) {
    int total_w = dot_strip_width();
    if (x < strip_x || x >= strip_x + total_w) return -1;
    int dy = y - y_center;
    if (dy < -WS_DOT_RADIUS - 2 || dy > WS_DOT_RADIUS + 2) return -1;
    int local = x - strip_x;
    int slot = WS_DOT_RADIUS * 2 + WS_DOT_PAD;
    int idx = local / slot;
    if (idx < 0 || idx >= g_ws_count) return -1;
    return idx;
}

/* ── Persistence ── */

#define WS_VAULT_STATE_KEY  "/wm/workspaces_state"
#define WS_VAULT_MAP_KEY    "/wm/window_workspace_map"
#define WS_SAVE_MAGIC  0x57535053u  /* 'WSPS' */

typedef struct {
    uint32_t magic;
    int32_t  count;
    int32_t  active;
    int32_t  anim_ms;
    char     names[WS_MAX_COUNT][WS_NAME_MAX];
    char     wallpapers[WS_MAX_COUNT][WS_WALLPAPER_MAX];
    int32_t  focused[WS_MAX_COUNT];
} workspaces_save_t;

/* Window→workspace map. Saved as a list of (chain_id, workspace_id)
 * pairs. We use chain_id as the stable key; surface ids are not stable
 * across reboots, but the chain a surface renders is. */
typedef struct {
    int32_t chain_id;
    int32_t workspace;
} ws_map_entry_t;

typedef struct {
    uint32_t magic;
    int32_t  count;
    ws_map_entry_t entries[WS_MAX_WINDOWS * WS_MAX_COUNT];
} ws_map_save_t;

#define WS_MAP_MAGIC 0x57534D50u  /* 'WSMP' */

void workspaces_save(void) {
    workspaces_save_t s;
    s.magic = WS_SAVE_MAGIC;
    s.count = g_ws_count;
    s.active = g_ws_active;
    s.anim_ms = g_anim_ms;
    for (int i = 0; i < WS_MAX_COUNT; i++) {
        str_copy(s.names[i], g_ws[i].name, WS_NAME_MAX);
        str_copy(s.wallpapers[i], g_ws[i].wallpaper_path, WS_WALLPAPER_MAX);
        s.focused[i] = g_ws[i].focused_window_id;
    }
    vault_save_config(WS_VAULT_STATE_KEY, &s, sizeof(s));

    /* Map */
    ws_map_save_t m;
    m.magic = WS_MAP_MAGIC;
    m.count = 0;
    int n = wm_surface_count();
    for (int i = 0; i < n && m.count < (int)(sizeof(m.entries) / sizeof(m.entries[0])); i++) {
        chain_surface_t *cs = wm_get_surface_by_index(i);
        if (!cs) continue;
        m.entries[m.count].chain_id = cs->chain_id;
        m.entries[m.count].workspace = cs->workspace;
        m.count++;
    }
    vault_save_config(WS_VAULT_MAP_KEY, &m, sizeof(m));
}

void workspaces_load(void) {
    workspaces_save_t s;
    int got = vault_load_config(WS_VAULT_STATE_KEY, &s, sizeof(s));
    if (got > 0 && s.magic == WS_SAVE_MAGIC &&
        s.count >= 1 && s.count <= WS_MAX_COUNT)
    {
        g_ws_count = s.count;
        g_ws_active = clamp_int(s.active, 0, g_ws_count - 1);
        g_anim_ms = clamp_int(s.anim_ms, 30, 2000);
        for (int i = 0; i < WS_MAX_COUNT; i++) {
            str_copy(g_ws[i].name, s.names[i], WS_NAME_MAX);
            if (g_ws[i].name[0] == 0)
                str_default_ws_name(g_ws[i].name, i);
            str_copy(g_ws[i].wallpaper_path, s.wallpapers[i], WS_WALLPAPER_MAX);
            g_ws[i].focused_window_id = s.focused[i];
        }
        kputs("WS: loaded state (count=");
        kput_dec((uint64_t)g_ws_count);
        kputs(")\n");
    }

    /* Apply window→workspace map. The WM is brought up before any
     * windows exist on a fresh boot, so this is a no-op there. After
     * windows are registered (e.g. via desktop launches replayed by
     * persistence), workspaces_window_assigned() keeps things in sync. */
    ws_map_save_t m;
    int gm = vault_load_config(WS_VAULT_MAP_KEY, &m, sizeof(m));
    if (gm > 0 && m.magic == WS_MAP_MAGIC && m.count >= 0) {
        int n = wm_surface_count();
        for (int i = 0; i < n; i++) {
            chain_surface_t *cs = wm_get_surface_by_index(i);
            if (!cs) continue;
            for (int e = 0; e < m.count; e++) {
                if (m.entries[e].chain_id == cs->chain_id) {
                    int ws = m.entries[e].workspace;
                    if (ws >= 0 && ws < g_ws_count)
                        cs->workspace = ws;
                    break;
                }
            }
        }
    }

    workspaces_rebuild_lists();
}

int workspaces_total_windows(void) {
    int total = 0;
    for (int i = 0; i < g_ws_count; i++)
        total += g_ws[i].window_count;
    return total;
}
