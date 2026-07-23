/*
 * Zeos — Dock
 *
 * Bottom-edge launcher bar. Pinned items on the left, running chains
 * on the right, separated by a vertical divider. Spring-animated
 * auto-hide. Rendered by the compositor at Layer 3.
 */

#include "dock.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "anim.h"
#include "wm.h"
#include "chain.h"
#include "compositor.h"
#include "kprint.h"
#include "ui_hover.h"
#include "ui_context_menu.h"
#include "icon_render.h"

/* ── UI primitive wiring ── */
#define DOCK_MAX_HOVERS  (DOCK_MAX_PINNED + DOCK_MAX_RUNNING)
static uint64_t s_dock_tokens[DOCK_MAX_HOVERS];
static int      s_dock_token_count = 0;
static int      s_rc_kind = -1;
static int      s_rc_idx  = -1;

/* ── Constants ── */
/* All metrics snap to the Z* spacing scale (theme.h). */
#define DOCK_ITEM_SIZE    (Z12 + Z3)    /* 60px — bigger icons */
#define DOCK_ITEM_PAD      Z2           /* 8px */
#define DOCK_MARGIN        Z6           /* 24px */
#define DOCK_DIVIDER_W     1            /* 1px hairline (separator thickness) */
#define DOCK_DIVIDER_GAP   Z2           /* 8px — Z2 */
#define DOCK_DOT_RADIUS    3            /* 3px state dot */
#define DOCK_CORNER_R      Z2           /* 8px corner radius — BORDER_RADIUS */
#define DOCK_HEIGHT       (Z16 + Z6)    /* 88px — taller, more prominent */

static dock_state_t g_dock;

/* ── Helpers ── */

static void str_copy(char *d, const char *s, int max) {
    int i = 0;
    while (i < max - 1 && s[i]) {
        d[i] = s[i];
        i++;
    }
    d[i] = 0;
}

static int total_item_count(void) {
    return g_dock.pinned_count + g_dock.running_count;
}

/* Compute dock width based on current item count */
static int compute_dock_width(void) {
    int n = total_item_count();
    if (n == 0) return 0;

    int items_w = n * DOCK_ITEM_SIZE + (n - 1) * DOCK_ITEM_PAD;

    /* Add divider space if we have both pinned and running */
    int divider_w = 0;
    if (g_dock.pinned_count > 0 && g_dock.running_count > 0)
        divider_w = DOCK_DIVIDER_GAP * 2 + DOCK_DIVIDER_W;

    return DOCK_MARGIN * 2 + items_w + divider_w;
}

/* ── Spring callback ── */

static void dock_slide_cb(int anim_id, float position, void *ctx) {
    (void)anim_id;
    (void)ctx;
    g_dock.slide_y = position;

    /* Update compositor visibility */
    if (position > 0.01f) {
        g_dock.visible = 1;
        compositor_show_dock(1);
    } else {
        g_dock.visible = 0;
        compositor_show_dock(0);
    }
    compositor_dirty_all();
}

/* ── API ── */

void dock_init(int auto_hide) {
    g_dock.visible = 0;
    g_dock.auto_hide = auto_hide;
    g_dock.slide_y = 0.0f;
    g_dock.anim_id = -1;
    g_dock.hover = 0;
    g_dock.pinned_count = 0;
    g_dock.running_count = 0;
    g_dock.selected = -1;
    g_dock.dock_h = DOCK_HEIGHT;
    g_dock.dock_w = 0;

    kputs("DOCK: initialized, auto_hide=");
    kput_dec((uint64_t)auto_hide);
    kputs("\n");
}

int dock_pin(const char *name, int chain_id) {
    if (g_dock.pinned_count >= DOCK_MAX_PINNED) return -1;

    dock_item_t *item = &g_dock.pinned[g_dock.pinned_count++];
    item->type = DOCK_ITEM_PINNED;
    str_copy(item->name, name, 32);
    item->chain_id = chain_id;
    item->surface_id = -1;
    item->accent = COLOR_PRIMARY;
    item->has_state_dot = 0;
    item->state = CHAIN_DETACHED;

    g_dock.dock_w = compute_dock_width();
    compositor_dirty_all();
    return 0;
}

void dock_unpin(int index) {
    if (index < 0 || index >= g_dock.pinned_count) return;

    /* Shift remaining items left */
    for (int i = index; i < g_dock.pinned_count - 1; i++)
        g_dock.pinned[i] = g_dock.pinned[i + 1];

    g_dock.pinned_count--;
    g_dock.dock_w = compute_dock_width();
    compositor_dirty_all();
}

void dock_update(void) {
    /* Clear running list and rebuild from WM surfaces. Pinned items
     * are global; running indicators are filtered to the active
     * workspace so each workspace shows only its own apps. */
    g_dock.running_count = 0;

    int count = wm_surface_count();
    int active_ws = wm_get_workspace();
    for (int i = 0; i < count && g_dock.running_count < DOCK_MAX_RUNNING; i++) {
        chain_surface_t *s = wm_get_surface_by_index(i);
        if (!s) continue;
        if (s->workspace != active_ws) continue;

        dock_item_t *item = &g_dock.running[g_dock.running_count++];
        item->type = DOCK_ITEM_RUNNING;
        str_copy(item->name, s->title, 32);
        item->chain_id = s->chain_id;
        item->surface_id = s->id;
        item->accent = s->accent;
        item->has_state_dot = 1;

        /* Map signal status to chain status for the dot */
        switch (s->signal) {
            case SIGNAL_LIVE:     item->state = CHAIN_LIVE;     break;
            case SIGNAL_PAUSED:   item->state = CHAIN_PAUSED;   break;
            case SIGNAL_ERROR:    item->state = CHAIN_ERROR;     break;
            case SIGNAL_DETACHED: item->state = CHAIN_DETACHED;  break;
        }
    }

    g_dock.dock_w = compute_dock_width();
}

void dock_show(void) {
    if (g_dock.anim_id >= 0 && anim_is_active(g_dock.anim_id))
        anim_retarget(g_dock.anim_id, 1.0f);
    else
        g_dock.anim_id = anim_spring(g_dock.slide_y, 1.0f,
                                      SPRING_SMOOTH_S, SPRING_SMOOTH_D,
                                      dock_slide_cb, 0);

    g_dock.visible = 1;
    compositor_show_dock(1);
}

/* Force the dock to its fully-slid-in state (skip the slide animation), for
 * synchronous paints before the anim tick loop is live. */
void dock_force_open(void) {
    g_dock.dock_w = compute_dock_width();
    g_dock.slide_y = 1.0f;
    g_dock.visible = 1;
    compositor_show_dock(1);
}

void dock_hide(void) {
    if (g_dock.anim_id >= 0 && anim_is_active(g_dock.anim_id))
        anim_retarget(g_dock.anim_id, 0.0f);
    else
        g_dock.anim_id = anim_spring(g_dock.slide_y, 0.0f,
                                      SPRING_SMOOTH_S, SPRING_SMOOTH_D,
                                      dock_slide_cb, 0);
}

void dock_toggle(void) {
    if (g_dock.visible)
        dock_hide();
    else
        dock_show();
}

void dock_mouse_enter(void) {
    g_dock.hover = 1;
    if (g_dock.auto_hide)
        dock_show();
}

void dock_mouse_leave(void) {
    g_dock.hover = 0;
    if (g_dock.auto_hide)
        dock_hide();
}

void dock_click(int x) {
    if (!g_dock.visible || g_dock.dock_w == 0) return;

    compositor_t *comp = compositor_get_state();
    int dx = (comp->screen_w - g_dock.dock_w) / 2;

    /* Local x within the dock */
    int lx = x - dx - DOCK_MARGIN;
    if (lx < 0) return;

    /* Walk pinned items */
    int offset = 0;
    for (int i = 0; i < g_dock.pinned_count; i++) {
        if (lx >= offset && lx < offset + DOCK_ITEM_SIZE) {
            /* Pinned item clicked — check if it has a running surface */
            dock_item_t *item = &g_dock.pinned[i];
            if (item->surface_id >= 0)
                wm_focus_surface(item->surface_id);
            /* Otherwise: would launch the chain (future) */
            return;
        }
        offset += DOCK_ITEM_SIZE + DOCK_ITEM_PAD;
    }

    /* Skip divider space */
    if (g_dock.pinned_count > 0 && g_dock.running_count > 0)
        offset += DOCK_DIVIDER_GAP * 2 + DOCK_DIVIDER_W;

    /* Walk running items */
    for (int i = 0; i < g_dock.running_count; i++) {
        if (lx >= offset && lx < offset + DOCK_ITEM_SIZE) {
            dock_item_t *item = &g_dock.running[i];
            if (item->surface_id >= 0)
                wm_focus_surface(item->surface_id);
            return;
        }
        offset += DOCK_ITEM_SIZE + DOCK_ITEM_PAD;
    }
}

/* ── Drawing ── */

/* Draw a single rounded-top-corner rectangle */
static void draw_rounded_top_rect(int x, int y, int w, int h, int r, uint32_t color) {
    /* Main body below the rounded area */
    fb_rect(x, y + r, w, h - r, color);
    /* Top strip between corners */
    fb_rect(x + r, y, w - 2 * r, r, color);

    /* Top-left corner (quarter circle approximation with filled rects) */
    for (int ry = 0; ry < r; ry++) {
        /* Compute x offset for this row: x^2 + y^2 <= r^2 */
        /* Integer sqrt approximation */
        int dy = r - ry;
        int dx2 = r * r - dy * dy;
        int rx = 0;
        while ((rx + 1) * (rx + 1) <= dx2) rx++;
        int x0 = r - rx;
        fb_hline(x + x0, y + ry, rx, color);
    }

    /* Top-right corner */
    for (int ry = 0; ry < r; ry++) {
        int dy = r - ry;
        int dx2 = r * r - dy * dy;
        int rx = 0;
        while ((rx + 1) * (rx + 1) <= dx2) rx++;
        int x0 = w - r;
        fb_hline(x + x0, y + ry, rx, color);
    }
}

/* Fully-rounded (all four corners) alpha-blended fill. */
static void fill_rounded_blend(int x, int y, int w, int h, int r, uint32_t argb) {
    if (r < 0) r = 0;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    fb_rect_blend(x, y + r, w, h - 2 * r, argb);            /* middle band */
    fb_rect_blend(x + r, y, w - 2 * r, r, argb);            /* top strip */
    fb_rect_blend(x + r, y + h - r, w - 2 * r, r, argb);    /* bottom strip */
    for (int ry = 0; ry < r; ry++) {
        int dy = r - ry;
        int dx2 = r * r - dy * dy;
        int rx = 0;
        while ((rx + 1) * (rx + 1) <= dx2) rx++;
        fb_rect_blend(x + (r - rx), y + ry,         rx, 1, argb);   /* TL */
        fb_rect_blend(x + w - r,    y + ry,         rx, 1, argb);   /* TR */
        fb_rect_blend(x + (r - rx), y + h - 1 - ry, rx, 1, argb);   /* BL */
        fb_rect_blend(x + w - r,    y + h - 1 - ry, rx, 1, argb);   /* BR */
    }
}

/* Same rounded-top shape, but alpha-blended — used to build a soft glow whose
 * corners follow the dock's rounding instead of a hard rectangular halo. */
static void fill_rounded_top_blend(int x, int y, int w, int h, int r, uint32_t argb) {
    if (r < 0) r = 0;
    if (r > h) r = h;
    fb_rect_blend(x, y + r, w, h - r, argb);         /* body */
    fb_rect_blend(x + r, y, w - 2 * r, r, argb);     /* top strip between corners */
    for (int ry = 0; ry < r; ry++) {                 /* rounded top corners */
        int dy = r - ry;
        int dx2 = r * r - dy * dy;
        int rx = 0;
        while ((rx + 1) * (rx + 1) <= dx2) rx++;
        fb_rect_blend(x + (r - rx), y + ry, rx, 1, argb);   /* top-left */
        fb_rect_blend(x + w - r,    y + ry, rx, 1, argb);   /* top-right */
    }
}

/* Draw a state dot under an item */
static void draw_state_dot(int cx, int cy, chain_status_t state) {
    uint32_t color;
    switch (state) {
        case CHAIN_LIVE:     color = COLOR_SUCCESS;      break;
        case CHAIN_PAUSED:   color = COLOR_ON_SURFACE_4; break;
        case CHAIN_ERROR:    color = COLOR_DANGER;        break;
        case CHAIN_DETACHED: return;  /* No dot for detached */
    }
    fb_circle_filled(cx, cy, DOCK_DOT_RADIUS, color);
}

/* Draw a single dock item cell */
/* Map an app/pin name to one of the real vector icons (icon_render.c). */
static icon_id_t icon_for_name(const char *name) {
    if (!name) return ICON_CHAIN;
    char c = name[0];
    if (c >= 'a' && c <= 'z') c -= 32;
    switch (c) {
        case 'F': return (name[1] == 'o' || name[1] == 'O') ? ICON_FOLDER : ICON_FOLDER; /* Files/Folder */
        case 'E': return ICON_PENCIL;      /* Editor */
        case 'T': return (name[1] == 'r' || name[1] == 'R') ? ICON_TRASH : ICON_TERMINAL;
        case 'S': return ICON_SETTINGS;    /* Settings */
        case 'C': return ICON_CHAIN;       /* Calculator/Chain */
        case 'B': return ICON_BROWSER;     /* Browser */
        case 'N': return ICON_NETWORK;
        case 'A': return ICON_AUDIO;
        case 'U': return ICON_USER;
        default:  return ICON_CHAIN;
    }
}

static void draw_item(int x, int y, dock_item_t *item, int is_selected) {
    /* Is this the app whose window currently has focus? */
    int focused = (item->surface_id >= 0 && item->surface_id == wm_get_focused());
    uint32_t accent = COLOR_PRIMARY;

    /* Focus highlight: a rounded accent pad behind the cell, plus a soft
     * accent glow ring so the active app clearly "lifts". */
    if (focused) {
        for (int i = 3; i >= 1; i--) {
            int e = i * 2;
            fill_rounded_blend(x - e, y - e, DOCK_ITEM_SIZE + 2 * e,
                               DOCK_ITEM_SIZE + 2 * e, 12 + e,
                               (accent & 0x00FFFFFF) | 0x14000000);
        }
        fill_rounded_blend(x - 4, y - 4, DOCK_ITEM_SIZE + 8, DOCK_ITEM_SIZE + 8,
                           12, (accent & 0x00FFFFFF) | 0x33000000);   /* ~20% pad */
    } else if (is_selected) {
        fb_rect_blend(x, y, DOCK_ITEM_SIZE, DOCK_ITEM_SIZE,
                      (item->accent & 0x00FFFFFF) | 0x3D000000);
    }

    /* Real vector app icon, centered in the cell. Focused = accent-tinted. */
    int isz = (DOCK_ITEM_SIZE * 3) / 5;                 /* icon fills ~60% of cell */
    int ix = x + (DOCK_ITEM_SIZE - isz) / 2;
    int iy = y + (DOCK_ITEM_SIZE - isz) / 2 - 4;        /* shift up for the indicator */
    /* Focused icon is bright white so it POPS against the accent pad instead
     * of blending into it (accent-on-accent read as "lost"). */
    uint32_t icol = focused ? 0xFFFFFFFFu
                  : is_selected ? item->accent
                  : COLOR_ON_SURFACE;
    icon_draw(icon_for_name(item->name), ix, iy, isz, icol);

    /* Active indicator: focused app gets a bright accent bar; other running
     * apps keep the small state dot. */
    if (focused) {
        int bw = DOCK_ITEM_SIZE / 3;
        int bx = x + (DOCK_ITEM_SIZE - bw) / 2;
        int by = y + DOCK_ITEM_SIZE - 4;
        fill_rounded_blend(bx, by, bw, 3, 1, accent);
    } else if (item->has_state_dot) {
        int dot_cx = x + DOCK_ITEM_SIZE / 2;
        int dot_cy = y + DOCK_ITEM_SIZE - DOCK_DOT_RADIUS - 2;
        draw_state_dot(dot_cx, dot_cy, item->state);
    }
}

void dock_draw(void) {
    if (!g_dock.visible || g_dock.dock_w == 0) return;

    compositor_t *comp = compositor_get_state();
    int screen_w = comp->screen_w;
    int screen_h = comp->screen_h;

    /* Compute slide offset: slide_y goes 0→1, dock slides up from below */
    int full_h = g_dock.dock_h;
    int slide_offset = (int)((1.0f - g_dock.slide_y) * (float)full_h);
    int dy = screen_h - full_h + slide_offset;

    int dx = (screen_w - g_dock.dock_w) / 2;

    /* Soft white glow behind the dock so it lifts off the wallpaper. Expanding
     * rounded-top layers (corner radius grows with each ring) -> the halo is
     * soft AND follows the dock's rounded corner instead of a boxy rectangle. */
    for (int i = 10; i >= 1; i--) {
        int e = i;                           /* short halo: ~10px, 1px steps */
        fill_rounded_top_blend(dx - e, dy - e, g_dock.dock_w + 2 * e, full_h + e,
                               DOCK_CORNER_R + e, 0x06FFFFFFu);  /* softer */
    }

    /* Background with rounded top corners — brighter surface for contrast. */
    draw_rounded_top_rect(dx, dy, g_dock.dock_w, full_h, DOCK_CORNER_R,
                          COLOR_SURFACE_TOP);

    /* Bright top edge — the "light" catching the top of the bar (blended). */
    fb_rect_blend(dx + DOCK_CORNER_R, dy, g_dock.dock_w - 2 * DOCK_CORNER_R, 1,
                  0x50FFFFFFu);
    fb_hline(dx + DOCK_CORNER_R, dy + 1, g_dock.dock_w - 2 * DOCK_CORNER_R,
             COLOR_SEPARATOR);

    /* Refresh hover zones — every visible cell gets one. */
    for (int i = 0; i < s_dock_token_count; i++) hover_unregister(s_dock_tokens[i]);
    s_dock_token_count = 0;

    /* Item rendering area */
    int item_y = dy + (full_h - DOCK_ITEM_SIZE) / 2;
    int item_x = dx + DOCK_MARGIN;
    int global_idx = 0;

    /* Draw pinned items */
    for (int i = 0; i < g_dock.pinned_count; i++) {
        int sel = (g_dock.selected == global_idx);
        draw_item(item_x, item_y, &g_dock.pinned[i], sel);
        if (s_dock_token_count < DOCK_MAX_HOVERS) {
            uint64_t tok = hover_register(item_x, item_y, DOCK_ITEM_SIZE, DOCK_ITEM_SIZE,
                                          HOVER_CURSOR_POINTER, 0, 0);
            if (tok) s_dock_tokens[s_dock_token_count++] = tok;
        }
        item_x += DOCK_ITEM_SIZE + DOCK_ITEM_PAD;
        global_idx++;
    }

    /* Vertical divider between pinned and running */
    if (g_dock.pinned_count > 0 && g_dock.running_count > 0) {
        item_x += DOCK_DIVIDER_GAP;
        int div_y1 = dy + 8;
        int div_y2 = dy + full_h - 8;
        fb_vline(item_x, div_y1, div_y2 - div_y1, COLOR_SEPARATOR);
        item_x += DOCK_DIVIDER_W + DOCK_DIVIDER_GAP;
    }

    /* Draw running items */
    for (int i = 0; i < g_dock.running_count; i++) {
        int sel = (g_dock.selected == global_idx);
        draw_item(item_x, item_y, &g_dock.running[i], sel);
        if (s_dock_token_count < DOCK_MAX_HOVERS) {
            uint64_t tok = hover_register(item_x, item_y, DOCK_ITEM_SIZE, DOCK_ITEM_SIZE,
                                          HOVER_CURSOR_POINTER, 0, 0);
            if (tok) s_dock_tokens[s_dock_token_count++] = tok;
        }
        item_x += DOCK_ITEM_SIZE + DOCK_ITEM_PAD;
        global_idx++;
    }
}

/* ── Right-click actions ── */
static void rc_quit(void *ctx) {
    (void)ctx;
    if (s_rc_kind == 1 && s_rc_idx >= 0 && s_rc_idx < g_dock.running_count) {
        int sid = g_dock.running[s_rc_idx].surface_id;
        if (sid >= 0) wm_minimize_surface(sid);
        kputs("DOCK: quit (minimize) sid="); kput_dec((uint64_t)sid); kputs("\n");
    }
}
static void rc_hide(void *ctx) {
    (void)ctx;
    if (s_rc_kind == 1 && s_rc_idx >= 0 && s_rc_idx < g_dock.running_count) {
        int sid = g_dock.running[s_rc_idx].surface_id;
        if (sid >= 0) wm_minimize_surface(sid);
    }
}
static void rc_pin(void *ctx) {
    (void)ctx;
    if (s_rc_kind == 1 && s_rc_idx >= 0 && s_rc_idx < g_dock.running_count) {
        dock_pin(g_dock.running[s_rc_idx].name, g_dock.running[s_rc_idx].chain_id);
    } else if (s_rc_kind == 0) {
        dock_unpin(s_rc_idx);
    }
}
static void rc_activity_monitor(void *ctx) {
    (void)ctx;
    extern int activity_open(void);
    (void)activity_open();
}

int dock_right_click(int x, int y) {
    if (!g_dock.visible) return 0;
    compositor_t *comp = compositor_get_state();
    int dx = (comp->screen_w - g_dock.dock_w) / 2;
    int slide_offset = (int)((1.0f - g_dock.slide_y) * (float)g_dock.dock_h);
    int dy = comp->screen_h - g_dock.dock_h + slide_offset;
    if (y < dy || y >= dy + g_dock.dock_h) return 0;

    int item_y = dy + (g_dock.dock_h - DOCK_ITEM_SIZE) / 2;
    (void)item_y;
    int item_x = dx + DOCK_MARGIN;
    for (int i = 0; i < g_dock.pinned_count; i++) {
        if (x >= item_x && x < item_x + DOCK_ITEM_SIZE) {
            s_rc_kind = 0; s_rc_idx = i;
            static const ctx_menu_item_t items[4] = {
                { "Quit",             rc_quit,             0, 0 },
                { "Hide",             rc_hide,             0, 0 },
                { "Unpin",            rc_pin,              0, 1 },
                { "Activity Monitor", rc_activity_monitor, 0, 1 },
            };
            context_menu_open(x, y, items, 4);
            return 1;
        }
        item_x += DOCK_ITEM_SIZE + DOCK_ITEM_PAD;
    }
    if (g_dock.pinned_count > 0 && g_dock.running_count > 0)
        item_x += DOCK_DIVIDER_GAP * 2 + DOCK_DIVIDER_W;
    for (int i = 0; i < g_dock.running_count; i++) {
        if (x >= item_x && x < item_x + DOCK_ITEM_SIZE) {
            s_rc_kind = 1; s_rc_idx = i;
            static const ctx_menu_item_t items[4] = {
                { "Quit",             rc_quit,             0, 1 },
                { "Hide",             rc_hide,             0, 1 },
                { "Pin",              rc_pin,              0, 1 },
                { "Activity Monitor", rc_activity_monitor, 0, 1 },
            };
            context_menu_open(x, y, items, 4);
            return 1;
        }
        item_x += DOCK_ITEM_SIZE + DOCK_ITEM_PAD;
    }
    /* Empty dock area — single-item context menu so right-click anywhere
     * on the dock surface still surfaces "Activity Monitor". */
    {
        s_rc_kind = -1; s_rc_idx = -1;
        static const ctx_menu_item_t items[1] = {
            { "Activity Monitor", rc_activity_monitor, 0, 1 },
        };
        context_menu_open(x, y, items, 1);
        return 1;
    }
}

int dock_get_height(void) {
    if (!g_dock.visible) return 0;
    return (int)(g_dock.slide_y * (float)g_dock.dock_h);
}

dock_state_t *dock_get_state(void) {
    return &g_dock;
}
