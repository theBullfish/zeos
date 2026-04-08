/*
 * Zeos — Compositor
 *
 * Renders all layers bottom-to-top into a back buffer,
 * then flips to the framebuffer. This prevents tearing
 * and lets us compose with alpha blending.
 */

#include "compositor.h"
#include "chain_registry.h"
#include "fb.h"
#include "wm.h"
#include "cursor.h"
#include "anim.h"
#include "timer.h"
#include "theme.h"
#include "font.h"
#include "heap.h"
#include "kprint.h"
#include "dock.h"
#include "panel.h"
#include "persona.h"
#include "desktop.h"
#include "hotcorners.h"
#include "mouse.h"
#include "notify.h"

static compositor_t g_comp;

/* ── Panel drawing (delegated to panel module) ── */

static void draw_panel(void) {
    if (!g_comp.panel_visible) return;
    panel_update();
    panel_draw();
}

/* ── Dock drawing ── */

static void draw_dock(void) {
    /* Delegate to the dock module — it handles visibility, animation, rendering */
    dock_update();
    dock_draw();
}

/* ── Desktop drawing ── */

static void draw_desktop(void) {
    /* Delegate to the desktop icon system — handles wallpaper + icons */
    desktop_draw();
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
    hotcorners_init(screen_w, screen_h);
    wm_init(screen_w, screen_h, g_comp.panel_h);
    panel_init(PERSONA_FULL, g_comp.panel_h);

    /* Wire all subsystems into the chain/MDE graph */
    int chains = chain_registry_init();
    if (chains < 0)
        kputs("COMP: WARNING -- chain registry init failed\n");
    else {
        kputs("COMP: chain graph wired (");
        kput_dec((uint64_t)chains);
        kputs(" chains)\n");
    }

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

    /* Tick hot corners (before animations — actions may trigger anim) */
    hotcorners_tick(mouse_get_x(), mouse_get_y());

    /* Tick animations */
    anim_tick(g_comp.frame_dt);

    /* Tick cursor animations */
    cursor_tick(g_comp.frame_dt);

    /*
     * ── Resolve chain graph ──
     * MDE resolves all chains in dependency order. This triggers
     * desktop_draw(), wm_draw_all(), panel_update()+panel_draw(),
     * dock_update()+dock_draw(), inspector_draw(), palette_draw()
     * through their chain node resolve functions.
     *
     * The chain graph replaces the manual layer calls below.
     * Layer visibility is still respected -- resolve functions
     * check the compositor state internally.
     */
    chain_registry_tick();

    /* ── Compose layers bottom to top ── */
    /*
     * NOTE: The chain graph now drives subsystem drawing via resolve
     * functions. The direct calls below are kept as a FALLBACK for
     * any layer that isn't yet chain-aware (cursor, snap ghost).
     * As subsystems move fully into chains, these will disappear.
     */

    /* Layer 0: Desktop -- driven by CHAIN_DESKTOP resolve */
    /* (desktop_draw called by chain resolution) */

    /* Layer 1: Chain surfaces -- driven by CHAIN_COMPOSITOR resolve */
    /* (wm_draw_all called by chain resolution) */

    /* Layer 2: Panel -- driven by CHAIN_PANEL resolve */
    /* (panel_update + panel_draw called by chain resolution) */

    /* Layer 3: Dock -- driven by CHAIN_DOCK resolve */
    /* (dock_update + dock_draw called by chain resolution) */

    /* Layer 4: Overlays (snap ghost already drawn by WM) */
    /* Inspector and palette are chain-driven now */

    /* Layer 4b: Notifications (overlay — tick + draw) */
    if (g_comp.layer_visible[COMP_LAYER_OVERLAYS]) {
        notify_tick();
        notify_draw();
    }

    /* Layer 5: Cursor (not yet a chain -- direct call) */
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
