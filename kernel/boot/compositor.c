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
#include "workspaces.h"
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
#include "image_viewer.h"
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

int compositor_init_ex(int screen_w, int screen_h, int wire_registry) {
    g_comp.screen_w = screen_w;
    g_comp.screen_h = screen_h;
    g_comp.panel_h = TOOLBAR_HEIGHT;
    g_comp.panel_visible = 1;
    g_comp.dock_h = 88;   /* matches DOCK_HEIGHT (dock.c) */
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
    workspaces_init(screen_w, screen_h);
    panel_init(PERSONA_FULL, g_comp.panel_h);

    /* Wire all subsystems into the chain/MDE graph. Skipped when the boot
     * path already initialized the registry (chain_registry_init resets it,
     * so calling it twice would wipe chains built after the first call). */
    if (wire_registry) {
        int chains = chain_registry_init();
        if (chains < 0)
            kputs("COMP: WARNING -- chain registry init failed\n");
        else {
            kputs("COMP: chain graph wired (");
            kput_dec((uint64_t)chains);
            kputs(" chains)\n");
        }
    }

    return 0;
}

int compositor_init(int screen_w, int screen_h) {
    return compositor_init_ex(screen_w, screen_h, 1);
}

/* ── Live desktop loop, split for scheduler_run epilogue calls ──
 * compositor_advance(): pre-resolve. Advances time-based state (anim/cursor/
 *   persona springs, overlay STATE ticks) EVERY tick, and re-arms compositing
 *   while any spring is live. Cheap when idle.
 * compositor_present(): post-resolve. Draws overlays + full-screen blends ONLY
 *   on a tick that actually composited (compositor_composited_this_tick()),
 *   then draws the cursor UNGATED on top every tick.
 * NOTE: neither calls chain_registry_tick() -- the old compositor_frame() did
 * (recursion trap). The scheduler drives chain_registry_tick() between these. */
void compositor_advance(void) {
    uint64_t now = timer_read_tsc();
    uint64_t freq = timer_tsc_freq();
    if (freq > 0)
        g_comp.frame_dt = (float)(now - g_comp.last_frame_tsc) / (float)freq;
    else
        g_comp.frame_dt = 1.0f / 60.0f;
    g_comp.last_frame_tsc = now;

    hotcorners_tick(mouse_get_x(), mouse_get_y());
    anim_tick(g_comp.frame_dt);
    cursor_tick(g_comp.frame_dt);
    persona_anim_tick();

    /* Overlay STATE ticks -- every tick, ungated, so toasts auto-dismiss and
     * app timers advance even when we skip the (expensive) composite. */
    if (g_comp.layer_visible[COMP_LAYER_OVERLAYS]) {
        notify_tick();
        { extern void editor_tick(void);   editor_tick();   }
        { extern void file_mgr_tick(void); file_mgr_tick(); }
        { extern void activity_tick(void); activity_tick(); }
    }

    /* Keep compositing while any spring is live; the gate closes when settled. */
    if (anim_active_count() > 0)
        compositor_dirty_all();

    /* Also recomposite while the left button is held -- window drag/resize
     * mutate geometry but push no dirty of their own. */
    {
        extern uint8_t mouse_get_buttons(void);
        if (mouse_get_buttons() & 0x01)   /* MOUSE_BTN_LEFT */
            compositor_dirty_all();
    }
}

void compositor_present(void) {
    /* FULL SCENE COMPOSITE, run DIRECTLY here (outside the watchdog-armed,
     * LAPIC-preemptible chain-resolve path where the 12-31ms wm_draw_all was
     * being longjmp-preempted mid-draw, so windows never finished and every
     * step after it was skipped). Gated on the dirty flag; when idle, the last
     * composited frame simply persists. Deterministic paint order, unpreemptible. */
    extern void desktop_draw(void);
    extern void panel_update(void);  extern void panel_draw(void);
    extern void dock_update(void);   extern void dock_draw(void);

    /* EXEMPT the composite from LAPIC-timer (vec 0xEF) preemption. present() runs
     * in the scheduler_run epilogue, but the one-shot preempt timer armed by the
     * last chain_resolve can still be counting -- if it fires here it longjmps out
     * AFTER desktop_draw() (wallpaper) but BEFORE wm_draw_all() finishes, leaving a
     * stable wallpaper-only frame (the windows "vanish"). Disarm it for the
     * composite; the scheduler re-arms per resolve next tick, so nothing to restore.
     * This is what makes the "unpreemptible" comment above actually true. */
    extern void lapic_timer_disarm(void);
    lapic_timer_disarm();

    int dirty = compositor_consume_dirty();
    if (dirty) {
        desktop_draw();       /* wallpaper + icons (bottom) */
        wm_draw_all();        /* windows */
        panel_update(); panel_draw();
        dock_update();  dock_draw();

        if (g_comp.layer_visible[COMP_LAYER_OVERLAYS]) {
            notify_draw();
            wm_draw_ghost();
            workspaces_overview_draw();
            context_menu_draw();
            quick_look_draw();
            image_viewer_draw();
            { extern int calculator_active(void); extern void calculator_draw(void);
              if (calculator_active()) calculator_draw(); }
            { extern int editor_active(void);     extern void editor_draw(void);
              if (editor_active()) editor_draw(); }
            { extern int file_mgr_active(void);   extern void file_mgr_draw(void);
              if (file_mgr_active()) file_mgr_draw(); }
            { extern int activity_active(void);   extern void activity_draw(void);
              if (activity_active()) activity_draw(); }
            dirty_modal_draw();
        }
        theme_apply_night_shift();
        { extern void idle_post_overlay(void); idle_post_overlay(); }
        g_comp.fully_dirty = 0;
    }

    /* Cursor: UNGATED, drawn on top every tick (it moves without a composite). */
    {
        extern int lockscreen_active(void);
        if (g_comp.layer_visible[COMP_LAYER_CURSOR] && !lockscreen_active())
            cursor_draw();
    }
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

/* Non-consuming peek of the dirty gate. Producers (desktop/dock/panel) gate
 * their draws on this so they don't repaint over windows on a skipped composite;
 * only compositor_mix_resolve CONSUMES (compositor_consume_dirty). */
int compositor_pending(void) { return (int)s_dirty_pending; }

/* Set by note_composite / cleared by note_skip each tick; read by
 * compositor_present() to decide whether overlays + full-screen blends run. */
static int s_composited_this_tick;
int compositor_composited_this_tick(void) { return s_composited_this_tick; }

/* Counters bumped by the chain resolve in chain_registry.c. */
void compositor_note_composite(void) { s_composite_count++; s_composited_this_tick = 1; }
void compositor_note_skip(void)      { s_composite_skips++; s_composited_this_tick = 0; }

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
