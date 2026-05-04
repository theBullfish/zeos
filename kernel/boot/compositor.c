/*
 * Zeos — Compositor
 *
 * Renders all layers bottom-to-top into a back buffer,
 * then flips to the framebuffer. This prevents tearing
 * and lets us compose with alpha blending.
 *
 * Async compositor: resolves at most once per N scheduler ticks
 * (default N=16, ~27 fps), and only if dirty rects exist. Per-display
 * vsync-rate decoupling means a 144Hz panel doesn't slow a 60Hz panel.
 *
 * Dirty rect tracking: subsystems push regions via compositor_dirty()
 * when their visual output changes (panel state, mouse move, dock
 * animation, notifications, etc.). The compositor's chain resolve
 * checks compositor_consume_dirty() and skips wm_draw_all() entirely
 * when nothing has changed. This is what drops average tick duration
 * from 12-31ms back to <2ms when the UI is idle.
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
#include "ui_context_menu.h"
#include "ui_dirty.h"
#include "quick_look.h"
#include "dock.h"
#include "panel.h"
#include "persona.h"
#include "desktop.h"
#include "hotcorners.h"
#include "mouse.h"
#include "notify.h"
#include "theme_runtime.h"
#include "persona_anim.h"

static compositor_t g_comp;

/* ── Dirty rect ring ──
 *
 * Subsystems push dirty rects via compositor_dirty(); the compositor's
 * resolve consumes them. The actual rect coordinates are not used yet
 * (we still redraw the full screen when ANY rect is present), but the
 * ring is the gating signal: zero pushed since last consume = skip
 * the entire composite pass.
 *
 * Future: track rect bounds to clip wm_draw_all() / panel_draw() /
 * etc. to only the changed pixels. For now the ring is a binary
 * "anything changed?" plus a count for instrumentation. */

#define COMP_DIRTY_RING 64

typedef struct { int x, y, w, h; } comp_dirty_rect_t;

static comp_dirty_rect_t s_dirty_ring[COMP_DIRTY_RING];
static uint32_t s_dirty_head;
static uint32_t s_dirty_pending;          /* unsumed count, capped at ring size */
static uint32_t s_dirty_pushes_total;     /* lifetime push count */
static uint32_t s_composite_count;        /* lifetime composites that ran */
static uint32_t s_composite_skips;        /* lifetime composites that were skipped */

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
    /* Ensure the first composite actually runs. */
    s_dirty_pending = 1;
    s_dirty_pushes_total = 1;
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

    /* Tick persona transition animation */
    persona_anim_tick();

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
        /* Layer 4c: UI overlays (context menu, quick look, dirty modal).
         * Drawn after notifications so a modal sits above a stale toast. */
        context_menu_draw();
        quick_look_draw();
        dirty_modal_draw();
    }

    /* Night shift: warm tint over all rendered content, before cursor */
    theme_apply_night_shift();

    /* Layer 5: Cursor (not yet a chain -- direct call). Suppressed
     * while the lock screen owns the modal -- the spec calls out
     * "cursor hidden" during LOCKED. */
    {
        extern int lockscreen_active(void);
        if (g_comp.layer_visible[COMP_LAYER_CURSOR] && !lockscreen_active())
            cursor_draw();
    }

    /* Layer 6: Idle/lock-on-idle post-overlay. Paints DIMMED tint or
     * the lock screen card on top of everything else. No-op while
     * IDLE_ACTIVE so this costs nothing in the steady state. */
    {
        extern void idle_post_overlay(void);
        idle_post_overlay();
    }

    g_comp.fully_dirty = 0;
}

void compositor_dirty(int x, int y, int w, int h) {
    g_comp.fully_dirty = 1;  /* full redraw until clipped paint lands */

    /* Push into ring. Bounded at COMP_DIRTY_RING so a flood doesn't
     * grow unbounded — extra pushes still bump pending so the
     * "anything dirty?" gate stays true. */
    s_dirty_ring[s_dirty_head].x = x;
    s_dirty_ring[s_dirty_head].y = y;
    s_dirty_ring[s_dirty_head].w = w;
    s_dirty_ring[s_dirty_head].h = h;
    s_dirty_head = (s_dirty_head + 1) % COMP_DIRTY_RING;
    if (s_dirty_pending < COMP_DIRTY_RING) s_dirty_pending++;
    s_dirty_pushes_total++;
}

void compositor_dirty_all(void) {
    g_comp.fully_dirty = 1;
    /* Use full-screen rect so consumers see a single pending entry. */
    compositor_dirty(0, 0, g_comp.screen_w, g_comp.screen_h);
}

int compositor_consume_dirty(void) {
    int n = (int)s_dirty_pending;
    s_dirty_pending = 0;
    return n;
}

uint32_t compositor_dirty_pushes(void)    { return s_dirty_pushes_total; }
uint32_t compositor_composite_count(void) { return s_composite_count; }
uint32_t compositor_composite_skips(void) { return s_composite_skips; }

/* Counters bumped by the chain resolve in chain_registry.c. */
void compositor_note_composite(void) { s_composite_count++; }
void compositor_note_skip(void)      { s_composite_skips++; }

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
