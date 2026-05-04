/*
 * Zeos -- Desktop Icons
 *
 * Grid-snapped icon system. Each icon is a colored square with
 * the first two letters of its name centered, plus the full name
 * as a caption below. Double-click launches the associated chain.
 *
 * All positions snap to the Z8 (32px) grid.
 * State persists via VAULT config.
 */

#include "desktop.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "vault.h"
#include "chain.h"
#include "wm.h"
#include "compositor.h"
#include "kprint.h"
#include "ui_context_menu.h"
#include "quick_look.h"

/* ── UI primitive wiring ── */
static void rc_new_folder(void *ctx) { (void)ctx; kputs("DESK: new folder\n"); }
static void rc_settings(void *ctx)   { (void)ctx; kputs("DESK: settings\n"); }
static void rc_wallpaper(void *ctx)  { (void)ctx; kputs("DESK: wallpaper\n"); }

int desktop_right_click(int x, int y) {
    (void)x; (void)y;
    static const ctx_menu_item_t items[3] = {
        { "New folder", rc_new_folder, 0, 1 },
        { "Settings",   rc_settings,   0, 1 },
        { "Wallpaper",  rc_wallpaper,  0, 1 },
    };
    context_menu_open(x, y, items, 3);
    return 1;
}

/* Spacebar on a list with a selected item → open Quick Look. The desktop
 * is the only built-in list with a notion of a "selected file path". */
static int desktop_quicklook_read(const char *path, uint8_t *out, int max,
                                  uint64_t *out_size, uint64_t *out_mtime,
                                  void *ctx)
{
    (void)path; (void)out; (void)max; (void)ctx;
    if (out_size)  *out_size  = 0;
    if (out_mtime) *out_mtime = 0;
    return 0;
}

void desktop_quick_look_selected(void) {
    extern desktop_state_t *desktop_get_state(void);
    desktop_state_t *st = desktop_get_state();
    for (int i = 0; i < st->icon_count; i++) {
        if (st->icons[i].selected) {
            quick_look_open(st->icons[i].name, 0, 0,
                            desktop_quicklook_read, 0);
            return;
        }
    }
}

/* ── Static state ── */
static desktop_state_t g_desktop;

/* ── VAULT persistence key ── */
#define DESKTOP_VAULT_KEY "desktop/icons"

/* ── Serialized icon for VAULT (fixed-size, no pointers) ── */
typedef struct {
    char     name[32];
    char     chain_type[32];
    int32_t  grid_x;
    int32_t  grid_y;
    uint32_t accent;
} desktop_icon_saved_t;

typedef struct {
    uint32_t magic;         /* 'DSKN' */
    int32_t  count;
    desktop_icon_saved_t icons[DESKTOP_MAX_ICONS];
} desktop_save_data_t;

#define DESKTOP_SAVE_MAGIC  0x44534B4E  /* 'DSKN' */

/* ── Helpers ── */

static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static int str_equal(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

/*
 * Convert pixel (x, y) to the icon index at that position, or -1.
 * Takes into account the panel offset at the top.
 */
static int icon_at(int px, int py) {
    compositor_t *comp = compositor_get_state();
    int panel_h = comp->panel_h;

    for (int i = 0; i < g_desktop.icon_count; i++) {
        desktop_icon_t *ic = &g_desktop.icons[i];
        int ix = ic->grid_x * g_desktop.grid_spacing;
        int iy = ic->grid_y * g_desktop.grid_spacing + panel_h;
        int total_h = g_desktop.icon_size + TYPE_CAPTION + 4;

        if (px >= ix && px < ix + g_desktop.icon_size &&
            py >= iy && py < iy + total_h) {
            return i;
        }
    }
    return -1;
}

/*
 * Blend an accent color at ~20% opacity onto a base color.
 * Simple channel math -- no float needed.
 * result = base + (accent - base) * 51/256
 */
static uint32_t blend_accent_20(uint32_t base, uint32_t accent) {
    uint32_t rb = (base  >> 16) & 0xFF, gb = (base  >> 8) & 0xFF, bb = base  & 0xFF;
    uint32_t ra = (accent >> 16) & 0xFF, ga = (accent >> 8) & 0xFF, ba = accent & 0xFF;

    uint32_t ro = rb + (((int)ra - (int)rb) * 51) / 256;
    uint32_t go = gb + (((int)ga - (int)gb) * 51) / 256;
    uint32_t bo = bb + (((int)ba - (int)bb) * 51) / 256;

    return 0xFF000000 | (ro << 16) | (go << 8) | bo;
}

/* ── API ── */

void desktop_init(uint32_t wallpaper_color, int density) {
    /* Zero everything */
    for (int i = 0; i < DESKTOP_MAX_ICONS; i++) {
        g_desktop.icons[i].name[0] = 0;
        g_desktop.icons[i].chain_type[0] = 0;
        g_desktop.icons[i].grid_x = 0;
        g_desktop.icons[i].grid_y = 0;
        g_desktop.icons[i].selected = 0;
        g_desktop.icons[i].accent = COLOR_PRIMARY;
    }
    g_desktop.icon_count = 0;
    g_desktop.wallpaper_color = wallpaper_color;
    g_desktop.grid_spacing = Z8;
    g_desktop.dragging = 0;
    g_desktop.drag_icon = -1;
    g_desktop.drag_x = 0;
    g_desktop.drag_y = 0;

    /* Icon size from density */
    switch (density) {
        case 0:  g_desktop.icon_size = 32; break;
        case 1:  g_desktop.icon_size = 40; break;
        case 2:  g_desktop.icon_size = 48; break;
        default: g_desktop.icon_size = 32; break;
    }

    /* Try to load saved icons from VAULT */
    desktop_load();

    kputs("DESK: initialized, ");
    kput_dec(g_desktop.icon_count);
    kputs(" icons, size=");
    kput_dec(g_desktop.icon_size);
    kputs("px\n");
}

int desktop_add_icon(const char *name, const char *chain_type,
                     int grid_x, int grid_y) {
    if (g_desktop.icon_count >= DESKTOP_MAX_ICONS)
        return -1;

    int idx = g_desktop.icon_count;
    desktop_icon_t *ic = &g_desktop.icons[idx];

    str_copy(ic->name, name, 32);
    str_copy(ic->chain_type, chain_type, 32);
    ic->grid_x = grid_x;
    ic->grid_y = grid_y;
    ic->selected = 0;
    ic->accent = COLOR_PRIMARY;

    g_desktop.icon_count++;
    return idx;
}

void desktop_remove_icon(int index) {
    if (index < 0 || index >= g_desktop.icon_count)
        return;

    /* Shift remaining icons down */
    for (int i = index; i < g_desktop.icon_count - 1; i++)
        g_desktop.icons[i] = g_desktop.icons[i + 1];

    g_desktop.icon_count--;

    /* Clear the last slot */
    desktop_icon_t *last = &g_desktop.icons[g_desktop.icon_count];
    last->name[0] = 0;
    last->chain_type[0] = 0;
    last->selected = 0;
}

void desktop_draw(void) {
    compositor_t *comp = compositor_get_state();
    int panel_h = comp->panel_h;
    int sw = comp->screen_w;
    int sh = comp->screen_h;

    /* Wallpaper fill */
    fb_rect(0, panel_h, sw, sh - panel_h, g_desktop.wallpaper_color);

    /* Subtle persona accent gradient at bottom (5% opacity) */
    uint32_t accent_faint = (COLOR_PRIMARY & 0x00FFFFFF) | 0x0D000000;
    int grad_h = 64;
    int grad_y = sh - grad_h;
    fb_rect_blend(0, grad_y, sw, grad_h, accent_faint);

    /* Draw each icon */
    for (int i = 0; i < g_desktop.icon_count; i++) {
        desktop_icon_t *ic = &g_desktop.icons[i];

        int ix, iy;

        /* If this icon is being dragged, use drag position */
        if (g_desktop.dragging && g_desktop.drag_icon == i) {
            ix = g_desktop.drag_x - g_desktop.icon_size / 2;
            iy = g_desktop.drag_y - g_desktop.icon_size / 2;
        } else {
            ix = ic->grid_x * g_desktop.grid_spacing;
            iy = ic->grid_y * g_desktop.grid_spacing + panel_h;
        }

        int sz = g_desktop.icon_size;

        /* Icon square: dim accent tint (20% overlay on surface color) */
        uint32_t icon_bg = blend_accent_20(COLOR_SURFACE_HIGH, ic->accent);
        fb_rect(ix, iy, sz, sz, icon_bg);

        /* Selected: 2px accent border */
        if (ic->selected) {
            fb_rect_outline(ix, iy, sz, sz, ic->accent, 2);
        }

        /* First two letters centered in the square */
        char initials[3] = { 0, 0, 0 };
        initials[0] = ic->name[0];
        if (ic->name[0] && ic->name[1])
            initials[1] = ic->name[1];

        int text_w = font_measure(initials, FONT_BOOT, TYPE_BODY);
        int text_h = font_line_height(FONT_BOOT, TYPE_BODY);
        int tx = ix + (sz - text_w) / 2;
        int ty = iy + (sz - text_h) / 2;
        font_draw(tx, ty, initials, FONT_BOOT, TYPE_BODY, COLOR_ON_SURFACE);

        /* Name caption below the icon */
        int name_w = font_measure(ic->name, FONT_BOOT, TYPE_CAPTION);
        int nx = ix + (sz - name_w) / 2;
        int ny = iy + sz + 2;

        /* Apply TEXT_PRIMARY alpha to white */
        uint32_t caption_color = (TEXT_PRIMARY << 24) | (COLOR_ON_SURFACE & 0x00FFFFFF);
        font_draw(nx, ny, ic->name, FONT_BOOT, TYPE_CAPTION, caption_color);
    }
}

void desktop_click(int x, int y) {
    /* Deselect all first */
    for (int i = 0; i < g_desktop.icon_count; i++)
        g_desktop.icons[i].selected = 0;

    /* Select the one under cursor */
    int idx = icon_at(x, y);
    if (idx >= 0)
        g_desktop.icons[idx].selected = 1;
}

void desktop_double_click(int x, int y) {
    int idx = icon_at(x, y);
    if (idx < 0)
        return;

    desktop_icon_t *ic = &g_desktop.icons[idx];

    /* Create a chain of the specified type */
    int chain_id = chain_create(ic->chain_type, -1, MASQ_INTERNAL);
    if (chain_id < 0) {
        kputs("DESK: failed to create chain '");
        kputs(ic->chain_type);
        kputs("'\n");
        return;
    }

    /* Open a surface for it */
    compositor_t *comp = compositor_get_state();
    int win_w = comp->screen_w / 2;
    int win_h = comp->screen_h / 2;
    int win_x = (comp->screen_w - win_w) / 2;
    int win_y = (comp->screen_h - win_h) / 2;

    int sid = wm_create_surface(ic->name, chain_id,
                                win_x, win_y, win_w, win_h, 0);
    if (sid >= 0) {
        wm_focus_surface(sid);
        kputs("DESK: launched '");
        kputs(ic->name);
        kputs("' -> chain ");
        kput_dec(chain_id);
        kputs(", surface ");
        kput_dec(sid);
        kputs("\n");
    }
}

void desktop_drag_start(int x, int y) {
    int idx = icon_at(x, y);
    if (idx < 0)
        return;

    g_desktop.dragging = 1;
    g_desktop.drag_icon = idx;
    g_desktop.drag_x = x;
    g_desktop.drag_y = y;
    g_desktop.icons[idx].selected = 1;
}

void desktop_drag_move(int x, int y) {
    if (!g_desktop.dragging)
        return;

    g_desktop.drag_x = x;
    g_desktop.drag_y = y;
}

void desktop_drag_end(int x, int y) {
    if (!g_desktop.dragging)
        return;

    compositor_t *comp = compositor_get_state();
    int panel_h = comp->panel_h;
    int idx = g_desktop.drag_icon;

    if (idx >= 0 && idx < g_desktop.icon_count) {
        /* Snap to grid */
        int gx = x / g_desktop.grid_spacing;
        int gy = (y - panel_h) / g_desktop.grid_spacing;

        /* Clamp to valid range */
        if (gx < 0) gx = 0;
        if (gy < 0) gy = 0;

        int max_gx = (comp->screen_w / g_desktop.grid_spacing) - 1;
        int max_gy = ((comp->screen_h - panel_h) / g_desktop.grid_spacing) - 1;
        if (gx > max_gx) gx = max_gx;
        if (gy > max_gy) gy = max_gy;

        g_desktop.icons[idx].grid_x = gx;
        g_desktop.icons[idx].grid_y = gy;
    }

    g_desktop.dragging = 0;
    g_desktop.drag_icon = -1;

    /* Persist new positions */
    desktop_save();
}

void desktop_save(void) {
    desktop_save_data_t data;
    data.magic = DESKTOP_SAVE_MAGIC;
    data.count = g_desktop.icon_count;

    for (int i = 0; i < g_desktop.icon_count; i++) {
        desktop_icon_t *ic = &g_desktop.icons[i];
        desktop_icon_saved_t *si = &data.icons[i];
        str_copy(si->name, ic->name, 32);
        str_copy(si->chain_type, ic->chain_type, 32);
        si->grid_x = ic->grid_x;
        si->grid_y = ic->grid_y;
        si->accent = ic->accent;
    }

    /* Clear unused slots */
    for (int i = g_desktop.icon_count; i < DESKTOP_MAX_ICONS; i++) {
        data.icons[i].name[0] = 0;
        data.icons[i].chain_type[0] = 0;
        data.icons[i].grid_x = 0;
        data.icons[i].grid_y = 0;
        data.icons[i].accent = 0;
    }

    int written = vault_save_config(DESKTOP_VAULT_KEY, &data, sizeof(data));
    if (written > 0) {
        kputs("DESK: saved ");
        kput_dec(g_desktop.icon_count);
        kputs(" icons to VAULT\n");
    }
}

void desktop_load(void) {
    desktop_save_data_t data;
    int read = vault_load_config(DESKTOP_VAULT_KEY, &data, sizeof(data));

    if (read <= 0 || data.magic != DESKTOP_SAVE_MAGIC)
        return;  /* No saved data or corrupt -- start empty */

    if (data.count < 0 || data.count > DESKTOP_MAX_ICONS)
        return;  /* Sanity check */

    g_desktop.icon_count = data.count;

    for (int i = 0; i < data.count; i++) {
        desktop_icon_saved_t *si = &data.icons[i];
        desktop_icon_t *ic = &g_desktop.icons[i];

        str_copy(ic->name, si->name, 32);
        str_copy(ic->chain_type, si->chain_type, 32);
        ic->grid_x = si->grid_x;
        ic->grid_y = si->grid_y;
        ic->accent = si->accent ? si->accent : COLOR_PRIMARY;
        ic->selected = 0;
    }

    kputs("DESK: loaded ");
    kput_dec(g_desktop.icon_count);
    kputs(" icons from VAULT\n");
}

desktop_state_t *desktop_get_state(void) {
    return &g_desktop;
}
