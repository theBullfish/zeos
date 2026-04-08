/*
 * Zeos — Compositor
 *
 * Renders all layers bottom-to-top into a back buffer,
 * then flips to the framebuffer. This prevents tearing
 * and lets us compose with alpha blending.
 */

#include "compositor.h"
#include "fb.h"
#include "wm.h"
#include "cursor.h"
#include "anim.h"
#include "timer.h"
#include "theme.h"
#include "font.h"
#include "heap.h"
#include "kprint.h"

static compositor_t g_comp;

/* ── Panel drawing ── */

static void draw_panel(void) {
    if (!g_comp.panel_visible) return;

    int pw = g_comp.screen_w;
    int ph = g_comp.panel_h;

    /* Panel background */
    fb_rect(0, 0, pw, ph, COLOR_SURFACE_HIGH);

    /* Bottom separator */
    fb_hline(0, ph - 1, pw, COLOR_SEPARATOR);

    /* Left: persona indicator */
    fb_circle_filled(16, ph / 2, 5, COLOR_PRIMARY);

    /* Center: running chain count */
    int chain_count = wm_surface_count();
    char buf[32];
    int bi = 0;
    if (chain_count >= 10) buf[bi++] = '0' + (chain_count / 10);
    buf[bi++] = '0' + (chain_count % 10);
    buf[bi++] = ' '; buf[bi++] = 'c'; buf[bi++] = 'h';
    buf[bi++] = 'a'; buf[bi++] = 'i'; buf[bi++] = 'n'; buf[bi++] = 's';
    buf[bi] = 0;

    int text_w = font_measure(buf, FONT_BOOT, TYPE_LABEL);
    font_draw(pw / 2 - text_w / 2, 8, buf, FONT_BOOT, TYPE_LABEL, COLOR_ON_SURFACE_2);

    /* Right: workspace indicator */
    int ws = wm_get_workspace();
    char ws_buf[4] = { 'W', '0' + (char)ws + 1, 0 };
    font_draw(pw - 40, 8, ws_buf, FONT_BOOT, TYPE_LABEL, COLOR_ON_SURFACE_3);
}

/* ── Dock drawing ── */

static void draw_dock(void) {
    if (!g_comp.dock_visible) return;

    int dw = 300;  /* Dock width (centered) */
    int dh = g_comp.dock_h;
    int dx = (g_comp.screen_w - dw) / 2;
    int dy = g_comp.screen_h - dh;

    /* Dock background with rounded top */
    fb_rect(dx, dy, dw, dh, COLOR_SURFACE_HIGH);
    fb_hline(dx, dy, dw, COLOR_SEPARATOR);

    /* TODO: render pinned items + running chain indicators */
}

/* ── Desktop drawing ── */

static void draw_desktop(void) {
    fb_rect(0, g_comp.panel_h, g_comp.screen_w,
            g_comp.screen_h - g_comp.panel_h, g_comp.wallpaper_color);

    /* Subtle persona accent gradient at bottom (5% opacity) */
    uint32_t accent_faint = (COLOR_PRIMARY & 0x00FFFFFF) | 0x0D000000;
    int grad_h = 64;
    int grad_y = g_comp.screen_h - grad_h;
    fb_rect_blend(0, grad_y, g_comp.screen_w, grad_h, accent_faint);
}

/* ── Frame composition ── */

int compositor_init(int screen_w, int screen_h) {
    g_comp.screen_w = screen_w;
    g_comp.screen_h = screen_h;
    g_comp.panel_h = TOOLBAR_HEIGHT;
    g_comp.panel_visible = 1;
    g_comp.dock_h = 48;
    g_comp.dock_visible = 0;  /* Auto-hidden by default */
    g_comp.dock_auto_hide = 1;
    g_comp.wallpaper_color = COLOR_SURFACE;
    g_comp.fully_dirty = 1;
    g_comp.target_fps = 60;
    g_comp.last_frame_tsc = timer_read_tsc();

    for (int i = 0; i < COMP_LAYER_COUNT; i++)
        g_comp.layer_visible[i] = 1;

    /* Back buffer allocation (optional — direct rendering for now) */
    g_comp.backbuf = 0;
    g_comp.backbuf_size = screen_w * screen_h * 4;

    /*
     * Double buffering: allocate back buffer from heap
     * g_comp.backbuf = (uint32_t *)kmalloc(g_comp.backbuf_size);
     * For now, render directly to framebuffer (simpler, slight tearing).
     */

    kputs("COMP: initialized ");
    kput_dec(screen_w);
    kputs("x");
    kput_dec(screen_h);
    kputs(" @ ");
    kput_dec(g_comp.target_fps);
    kputs("fps\n");

    /* Initialize subsystems */
    wm_init(screen_w, screen_h, g_comp.panel_h);

    return 0;
}

void compositor_frame(void) {
    /* Calculate dt */
    uint64_t now = timer_read_tsc();
    uint64_t freq = timer_tsc_freq();
    if (freq > 0)
        g_comp.frame_dt = (float)(now - g_comp.last_frame_tsc) / (float)freq;
    else
        g_comp.frame_dt = 1.0f / 60.0f;
    g_comp.last_frame_tsc = now;

    /* Tick animations */
    anim_tick(g_comp.frame_dt);

    /* Tick cursor animations */
    cursor_tick(g_comp.frame_dt);

    /* ── Compose layers bottom to top ── */

    /* Layer 0: Desktop */
    if (g_comp.layer_visible[COMP_LAYER_DESKTOP])
        draw_desktop();

    /* Layer 1: Chain surfaces (window manager) */
    if (g_comp.layer_visible[COMP_LAYER_SURFACES])
        wm_draw_all();

    /* Layer 2: Panel */
    if (g_comp.layer_visible[COMP_LAYER_PANEL])
        draw_panel();

    /* Layer 3: Dock */
    if (g_comp.layer_visible[COMP_LAYER_DOCK])
        draw_dock();

    /* Layer 4: Overlays (snap ghost already drawn by WM) */
    /* Command palette, notifications, etc. — TODO */

    /* Layer 5: Cursor */
    if (g_comp.layer_visible[COMP_LAYER_CURSOR])
        cursor_draw();

    g_comp.fully_dirty = 0;
}

void compositor_dirty(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
    g_comp.fully_dirty = 1;  /* Simple: just redraw everything for now */
}

void compositor_dirty_all(void) {
    g_comp.fully_dirty = 1;
}

void compositor_set_wallpaper(uint32_t color) {
    g_comp.wallpaper_color = color;
    g_comp.fully_dirty = 1;
}

void compositor_show_dock(int show) {
    g_comp.dock_visible = show;
    g_comp.fully_dirty = 1;
}

compositor_t *compositor_get_state(void) {
    return &g_comp;
}
