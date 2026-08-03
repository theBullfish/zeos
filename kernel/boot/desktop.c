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
#include "icon_svg.h"   /* D.9: persona-tinted desktop icons */
#include "wm.h"
#include "compositor.h"
#include "kprint.h"
#include "ui_context_menu.h"
#include "quick_look.h"
#include "image_viewer.h"
#include "lodepng/lodepng.h"   /* D.10: decode wallpaper PNG loaded from VAULT */

/* D.10: default wallpaper, embedded (SVG->480x270 RGBA PNG). Seeded into VAULT
 * at first boot, then always LOADED BACK from VAULT (vault_read) so the desktop
 * honors a user-replaceable /system/wallpaper.png. */
extern const unsigned char _binary_wallpaper_png_start[];
extern const unsigned char _binary_wallpaper_png_end[];
extern void lodepng_free(void *ptr);   /* not exposed in lodepng.h */

#define DESKTOP_WALLPAPER_PATH "/system/wallpaper.png"
#define DESKTOP_WP_FILEBUF     49152   /* VAULT max file: 12 direct * 4096 */

static unsigned char  s_wp_filebuf[DESKTOP_WP_FILEBUF];
static const uint8_t *g_wp_rgba = 0;   /* lodepng RGBA, row-major, 4 B/px */
static int            g_wp_w = 0, g_wp_h = 0;
static int            g_wp_from_vault = 0;  /* 1 = decoded from a VAULT read */

/* ── UI primitive wiring ── */
static void rc_new_folder(void *ctx) { (void)ctx; kputs("DESK: new folder\n"); }
/* J.4: element-scoped settings — right-clicking the desktop opens Settings
 * jumped to the page for that element (wallpaper/display lives on DISPLAY). */
static void rc_settings(void *ctx)   { (void)ctx; extern void settings_open(void); settings_open(); }
static void rc_wallpaper(void *ctx)  { (void)ctx; extern void settings_open_page(int);
                                       settings_open_page(0 /* SETTINGS_PAGE_DISPLAY */); }

/* "Open in Image Viewer" path target. Set when a right-click lands on an
 * icon whose name has a .png suffix — the menu item only renders in that
 * case, so leaking stale state across clicks is harmless. */
static char s_rc_image_path[256];

static int desktop_name_is_png(const char *name) {
    int n = 0; while (name[n]) n++;
    if (n < 4) return 0;
    const char *e = name + n - 4;
    char a = e[0], b = e[1], c = e[2], d = e[3];
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (c >= 'A' && c <= 'Z') c += 32;
    if (d >= 'A' && d <= 'Z') d += 32;
    return a == '.' && b == 'p' && c == 'n' && d == 'g';
}

static void rc_open_in_image_viewer(void *ctx) {
    (void)ctx;
    if (s_rc_image_path[0]) image_viewer_open(s_rc_image_path);
}

/* "Open in Editor" path target. Set when a right-click lands on an
 * icon whose name looks like a text file. Same staleness rule as
 * s_rc_image_path. */
static char s_rc_text_path[256];

/* True if `name` ends in one of the recognized text-file suffixes
 * (.txt .md .c .h .zp .json .conf .log). Case-insensitive. */
static int desktop_name_is_text(const char *name) {
    int n = 0; while (name[n]) n++;
    if (n < 2) return 0;
    /* Find last '.' */
    int dot = -1;
    for (int i = n-1; i >= 0; i--) { if (name[i] == '.') { dot = i; break; } }
    if (dot < 0) return 0;
    char ext[8]; int e = 0;
    for (int i = dot; i < n && e < 7; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        ext[e++] = c;
    }
    ext[e] = 0;
    const char *exts[] = { ".txt", ".md", ".c", ".h", ".zp",
                           ".json", ".conf", ".log", 0 };
    for (int i = 0; exts[i]; i++) {
        const char *x = exts[i];
        int k = 0; while (x[k] && ext[k] == x[k]) k++;
        if (x[k] == 0 && ext[k] == 0) return 1;
    }
    return 0;
}

static void rc_open_in_editor(void *ctx) {
    (void)ctx;
    extern int editor_open(const char *path);
    if (s_rc_text_path[0]) editor_open(s_rc_text_path);
}

int desktop_right_click(int x, int y) {
    (void)x; (void)y;
    /* Look for a selected icon whose name ends in .png — if found, the
     * menu adds "Open in Image Viewer" as the first item. */
    extern desktop_state_t *desktop_get_state(void);
    desktop_state_t *st = desktop_get_state();
    const char *png_name = 0;
    const char *text_name = 0;
    for (int i = 0; i < st->icon_count; i++) {
        if (!st->icons[i].selected) continue;
        if (desktop_name_is_png(st->icons[i].name) && !png_name)
            png_name = st->icons[i].name;
        else if (desktop_name_is_text(st->icons[i].name) && !text_name)
            text_name = st->icons[i].name;
    }

    if (text_name) {
        int o = 0;
        s_rc_text_path[o++] = '/';
        for (int i = 0; text_name[i] && o < 255; i++)
            s_rc_text_path[o++] = text_name[i];
        s_rc_text_path[o] = 0;
        static const ctx_menu_item_t items[5] = {
            { "Open in Editor", rc_open_in_editor, 0, 1 },
            { "-",          0,             0, 1 },
            { "New folder", rc_new_folder, 0, 1 },
            { "Settings",   rc_settings,   0, 1 },
            { "Wallpaper",  rc_wallpaper,  0, 1 },
        };
        context_menu_open(x, y, items, 5);
        return 1;
    }

    if (png_name) {
        /* Resolve to /<name>; the desktop's icon names map to root-level
         * FAT32 files in the current bring-up. */
        int o = 0;
        s_rc_image_path[o++] = '/';
        for (int i = 0; png_name[i] && o < 255; i++)
            s_rc_image_path[o++] = png_name[i];
        s_rc_image_path[o] = '\0';

        static const ctx_menu_item_t items[5] = {
            { "Open in Image Viewer", rc_open_in_image_viewer, 0, 1 },
            { "-",          0,             0, 1 },
            { "New folder", rc_new_folder, 0, 1 },
            { "Settings",   rc_settings,   0, 1 },
            { "Wallpaper",  rc_wallpaper,  0, 1 },
        };
        context_menu_open(x, y, items, 5);
        return 1;
    }

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
            /* PNG → open in the full image viewer; everything else falls
             * through to Quick Look (which only does a metadata card for
             * non-text/non-PNG anyway). */
            if (desktop_name_is_png(st->icons[i].name)) {
                char path[260];
                int o = 0;
                path[o++] = '/';
                for (int j = 0; st->icons[i].name[j] && o < 259; j++)
                    path[o++] = st->icons[i].name[j];
                path[o] = '\0';
                image_viewer_open(path);
                return;
            }
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

/* Grid inset (px) so icons + captions don't clip the top-left edges. MUST be
 * used identically by the draw, hit-test (icon_at), and drag-snap (desktop_drag_end)
 * -- otherwise a plain click on an icon snaps it to the wrong cell (D.8 bug). */
#define DESKTOP_GRID_MARGIN 16

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

    int margin = DESKTOP_GRID_MARGIN;
    for (int i = 0; i < g_desktop.icon_count; i++) {
        desktop_icon_t *ic = &g_desktop.icons[i];
        int ix = margin + ic->grid_x * g_desktop.grid_spacing;
        int iy = panel_h + margin + ic->grid_y * g_desktop.grid_spacing;
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

/* D.10: seed the embedded default into VAULT if absent, then load the wallpaper
 * back FROM VAULT and decode it. Sets g_wp_rgba/w/h on success; leaves them zero
 * (solid-color fallback) on any failure. Idempotent — safe to call once at init. */
static void desktop_load_wallpaper(void) {
    unsigned emb_len = (unsigned)(_binary_wallpaper_png_end -
                                  _binary_wallpaper_png_start);

    /* Read whatever is in VAULT at the wallpaper path. */
    int got = vault_read(DESKTOP_WALLPAPER_PATH, s_wp_filebuf,
                         sizeof(s_wp_filebuf));

    /* Not present (or empty) -> seed the embedded default, then re-read so the
     * decode path always runs against VAULT bytes (proves the load path). */
    if (got <= 0) {
        if (emb_len > 0 && emb_len <= sizeof(s_wp_filebuf)) {
            vault_write(DESKTOP_WALLPAPER_PATH,
                        _binary_wallpaper_png_start, emb_len);
            got = vault_read(DESKTOP_WALLPAPER_PATH, s_wp_filebuf,
                             sizeof(s_wp_filebuf));
        }
    }
    if (got <= 0) {
        kputs("DESK: wallpaper VAULT read failed, using solid color\n");
        return;
    }

    unsigned char *pixels = 0;
    unsigned w = 0, h = 0;
    unsigned err = lodepng_decode32(&pixels, &w, &h,
                                    s_wp_filebuf, (size_t)got);
    if (err || !pixels || w == 0 || h == 0) {
        if (pixels) lodepng_free(pixels);
        kputs("DESK: wallpaper decode failed, using solid color\n");
        return;
    }

    g_wp_rgba = pixels;   /* kept alive for the life of the desktop */
    g_wp_w = (int)w;
    g_wp_h = (int)h;
    g_wp_from_vault = 1;
    kputs("DESK: wallpaper loaded from VAULT ");
    kput_dec((uint64_t)w); kputs("x"); kput_dec((uint64_t)h);
    kputs(" ("); kput_dec((uint64_t)got); kputs(" bytes)\n");
}

int desktop_wallpaper_loaded(void) {
    return (g_wp_rgba != 0 && g_wp_w > 0 && g_wp_h > 0 && g_wp_from_vault) ? 1 : 0;
}

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

    /* D.9: seed default launcher icons when nothing was persisted (the desktop
     * was otherwise empty -- desktop_add_icon had no callers). Names map to
     * persona-tinted icons via icon_svg_for_name (Files->folder, Terminal->code,
     * Settings->gear). */
    if (g_desktop.icon_count == 0) {
        /* Every 2 grid cells vertically: the 32px grid unit is smaller than a
         * 48px icon + caption, so 1-apart would overlap. */
        desktop_add_icon("Files",    "files",    0, 0);
        desktop_add_icon("Terminal", "terminal", 0, 2);
        desktop_add_icon("Settings", "settings", 0, 4);
    }

    /* D.10: load the wallpaper image from VAULT (seeds the embedded default on
     * first boot). Falls back to wallpaper_color if unavailable. */
    desktop_load_wallpaper();

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

    /* Wallpaper: image from VAULT (D.10) nearest-neighbour scaled to fill, else
     * solid color. fb_pixel is clip-tested, so off-dirty-rect pixels are cheap
     * (the compositor only invokes us over dirty regions). */
    int desk_h = sh - panel_h;
    if (g_wp_rgba && g_wp_w > 0 && g_wp_h > 0 && desk_h > 0) {
        /* Only iterate the region that will actually be written this frame:
         * intersect the desktop area with the active clip (the compositor sets
         * a small dirty rect on partial redraws). Screen coords dy/dx map into
         * wallpaper source via nearest-neighbour. Without this the loop scans
         * all ~2M pixels every desktop_draw, dominating composite time. */
        int cx0, cy0, cx1, cy1;
        fb_get_clip(&cx0, &cy0, &cx1, &cy1);
        int y0 = panel_h,       y1 = sh;      /* desktop band in screen coords */
        int x0 = 0,             x1 = sw;
        if (cy0 > y0) y0 = cy0; if (cy1 < y1) y1 = cy1;
        if (cx0 > x0) x0 = cx0; if (cx1 < x1) x1 = cx1;
        for (int sy = y0; sy < y1; sy++) {
            int dy = sy - panel_h;
            int syi = dy * g_wp_h / desk_h;
            if (syi >= g_wp_h) syi = g_wp_h - 1;
            const uint8_t *srow = g_wp_rgba + (size_t)syi * g_wp_w * 4;
            for (int sx = x0; sx < x1; sx++) {
                int sxi = sx * g_wp_w / sw;
                if (sxi >= g_wp_w) sxi = g_wp_w - 1;
                const uint8_t *p = srow + (size_t)sxi * 4;
                uint32_t c = 0xFF000000u | ((uint32_t)p[0] << 16) |
                             ((uint32_t)p[1] << 8) | (uint32_t)p[2];
                fb_pixel(sx, sy, c);
            }
        }
    } else {
        fb_rect(0, panel_h, sw, sh - panel_h, g_desktop.wallpaper_color);
    }

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
            /* D.9: inset the grid so icons + their captions don't clip the
             * left/top edges (captions are centered under a square at x=0). */
            int margin = DESKTOP_GRID_MARGIN;
            ix = margin + ic->grid_x * g_desktop.grid_spacing;
            iy = panel_h + margin + ic->grid_y * g_desktop.grid_spacing;
        }

        int sz = g_desktop.icon_size;

        /* Icon square: dim accent tint (20% overlay on surface color) */
        uint32_t icon_bg = blend_accent_20(COLOR_SURFACE_HIGH, ic->accent);
        fb_rect(ix, iy, sz, sz, icon_bg);

        /* Selected: 2px accent border */
        if (ic->selected) {
            fb_rect_outline(ix, iy, sz, sz, ic->accent, 2);
        }

        /* D.9: draw the real persona-tinted icon when we have one; otherwise
         * fall back to the first-two-letters initials. */
        unsigned long ilen = 0;
        const unsigned char *ipng = icon_svg_for_name(ic->name, &ilen);
        if (ipng) {
            int ipx = sz - 12;              /* inset the glyph within the square */
            if (ipx < 8) ipx = sz;
            int gx = ix + (sz - ipx) / 2;
            int gy = iy + (sz - ipx) / 2;
            icon_svg_draw(ipng, ilen, gx, gy, ipx, ic->accent);
        } else {
            char initials[3] = { 0, 0, 0 };
            initials[0] = ic->name[0];
            if (ic->name[0] && ic->name[1])
                initials[1] = ic->name[1];

            int text_w = font_measure(initials, FONT_BOOT, TYPE_BODY);
            int text_h = font_line_height(FONT_BOOT, TYPE_BODY);
            int tx = ix + (sz - text_w) / 2;
            int ty = iy + (sz - text_h) / 2;
            font_draw(tx, ty, initials, FONT_BOOT, TYPE_BODY, COLOR_ON_SURFACE);
        }

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
        /* Snap to grid. Must use the same DESKTOP_GRID_MARGIN inset as the draw
         * and icon_at, or a click (which arms a drag) snaps the icon to the
         * wrong cell and moves it (D.8 bug). */
        int gx = (x - DESKTOP_GRID_MARGIN) / g_desktop.grid_spacing;
        int gy = (y - panel_h - DESKTOP_GRID_MARGIN) / g_desktop.grid_spacing;

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
