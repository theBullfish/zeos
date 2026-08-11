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
#include "access.h"   /* density -> panel height (M.3) */

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
static uint32_t s_composite_clipped;      /* composites that ran clipped (partial) */

/* ── B.5/B.9 delta union ──
 *
 * Bounding box of every region-delta pushed via compositor_dirty() since the
 * last composite. The correction pass clips drawing to this box (unless a full
 * redraw was requested via fully_dirty, or the box covers most of the screen,
 * in which case a full redraw is cheaper than the clip bookkeeping). */
static int s_delta_valid;
static int s_delta_x0, s_delta_y0, s_delta_x1, s_delta_y1;

#ifdef ZEOS_DIAG_B9_SELFCHECK
#define B9_SELFCHECK_FRAMES 20
static uint32_t s_b9_frames;
static uint32_t s_b9_mismatch;
static uint64_t s_b9_clip_ticks;   /* summed TSC of clipped corrections */
static uint64_t s_b9_full_ticks;   /* summed TSC of full redraws (same state) */
#endif

static void delta_reset(void) { s_delta_valid = 0; }

static void delta_add(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    int x1 = x + w, y1 = y + h;
    if (!s_delta_valid) {
        s_delta_x0 = x; s_delta_y0 = y; s_delta_x1 = x1; s_delta_y1 = y1;
        s_delta_valid = 1;
    } else {
        if (x  < s_delta_x0) s_delta_x0 = x;
        if (y  < s_delta_y0) s_delta_y0 = y;
        if (x1 > s_delta_x1) s_delta_x1 = x1;
        if (y1 > s_delta_y1) s_delta_y1 = y1;
    }
}

/* ── Panel drawing (delegated to panel module) ── */

static void draw_panel(void) {
    if (!g_comp.panel_visible) return;
    panel_update();
    /* Panel stands down for a truly fullscreen surface: the user asked for one
     * thing on the screen, and "fullscreen" that still has a clock across it is
     * not fullscreen. Same authority the network indicator uses. */
    { extern int wm_fullscreen_active(void); if (!wm_fullscreen_active()) panel_draw(); }
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
    g_comp.panel_h = access_get_panel_height();   /* M.3: density-driven (48/40/32) */
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

    /* B.6 double buffer: composite into a WB-cached back buffer (fast per-pixel
     * work), then flip the finished scene to the WC front in one bulk copy. */
    g_comp.backbuf = 0;
    g_comp.backbuf_size = screen_w * screen_h * 4;
    if (fb_backbuf_init() == 0)
        kputs("COMP: double buffer active (composite -> WC flip)\n");
    else
        kputs("COMP: double buffer unavailable -- direct render\n");

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

void compositor_set_panel_h(int h) { if (h >= 16) g_comp.panel_h = h; }

/* ── Live desktop loop, split for scheduler_run epilogue calls ──
 * compositor_advance(): pre-resolve. Advances time-based state (anim/cursor/
 *   persona springs, overlay STATE ticks) EVERY tick, and re-arms compositing
 *   while any spring is live. Cheap when idle.
 * compositor_present(): post-resolve. Draws overlays + full-screen blends ONLY
 *   on a tick that actually composited (compositor_composited_this_tick()),
 *   then draws the cursor UNGATED on top every tick.
 * NOTE: neither calls chain_registry_tick() -- the old compositor_frame() did
 * (recursion trap). The scheduler drives chain_registry_tick() between these. */
/* B.4 smoke-check: confirm frame_dt is on the TSC-derived path, NOT the
 * 1/60 hardcoded fallback. GATED out of production (fleet-review finding #5,
 * 2026-07-25) -- it was previously unconditional AND its wording overclaimed.
 * CORRECTED framing: this is NOT an independent measurement. Both the summed
 * frame_dt and the "delta" below come from the SAME timer_read_tsc()/freq, so
 * this cannot prove TSC *accuracy* vs true wall-clock -- it is arithmetically
 * a telescoping sum. What it CAN show (real_us is computed from raw TSC
 * regardless of which path frame_dt took): if frame_dt had silently fallen
 * back to 1/60, the sum would be DT_SELFTEST_WINDOW*16666us and would NOT
 * match real_us -- so a match rules out the fallback and confirms frame_dt is
 * genuinely TSC-derived. NOTE the real discriminator is structural, not the
 * sum: the block is gated on freq>0 (the same condition that selects the TSC
 * branch for frame_dt), so the mere EMISSION of this line proves the TSC path
 * ran this boot, and real_us (~36ms/frame here) proves the TSC is calibrated and
 * running -- the sum==real_us match is a telescoping identity (diff structurally
 * ~0) and the MISMATCH branch is only a belt-and-suspenders guard. Un-gated: it
 * runs once (DT_SELFTEST_WINDOW frames) early at boot on the production path and
 * prints the [compositor] B.4 smoke-check line to serial. */
/* B.4 smoke-check state — un-gated for production: it self-terminates after
 * DT_SELFTEST_WINDOW compositor frames (once, early at boot) and thereafter costs
 * one already-computed bool check per frame. */
static uint32_t s_dt_selftest_ticks = 0;
static float    s_dt_selftest_sum = 0.0f;
static uint64_t s_dt_selftest_start_tsc = 0;
static int      s_dt_selftest_done = 0;
#define DT_SELFTEST_WINDOW 3

void compositor_advance(void) {
    uint64_t now = timer_read_tsc();
    uint64_t freq = timer_tsc_freq();
    if (freq > 0)
        g_comp.frame_dt = (float)(now - g_comp.last_frame_tsc) / (float)freq;
    else
        g_comp.frame_dt = 1.0f / 60.0f;
    g_comp.last_frame_tsc = now;

    if (!s_dt_selftest_done && freq > 0) {
        if (s_dt_selftest_start_tsc == 0) {
            s_dt_selftest_start_tsc = now;   /* baseline: second call onward */
        } else {
            s_dt_selftest_sum += g_comp.frame_dt;
            s_dt_selftest_ticks++;
            if (s_dt_selftest_ticks == DT_SELFTEST_WINDOW) {
                uint64_t real_us = (now - s_dt_selftest_start_tsc) * 1000000ULL / freq;
                uint32_t sum_us = (uint32_t)(s_dt_selftest_sum * 1000000.0f);
                uint32_t fallback_us = DT_SELFTEST_WINDOW * 16666u;
                /* Compare with a small tolerance: sum_us is a sum of float
                 * frame_dt values, so it accrues sub-us float rounding vs the
                 * single integer TSC delta (e.g. 96037 vs 96036). Exact equality
                 * false-alarmed. The real signal is TSC-path vs 1/60 fallback,
                 * which differ by ~46000us here -- a few us of drift is a MATCH. */
                uint32_t diff_us = (sum_us > (uint32_t)real_us)
                                 ? sum_us - (uint32_t)real_us
                                 : (uint32_t)real_us - sum_us;
                int on_tsc_path = (diff_us <= 100u);   /* << the ~46000us fallback gap */
                kputs("[compositor] B.4 smoke-check: summed frame_dt=");
                kput_dec(sum_us);
                kputs("us vs raw-TSC delta=");
                kput_dec(real_us);
                kputs("us (1/60 fallback would read ~");
                kput_dec((uint64_t)fallback_us);
                kputs("us, diff=");
                kput_dec((uint64_t)diff_us);
                kputs("us) -> ");
                kputs(on_tsc_path ? "on TSC path\n" : "MISMATCH (check fallback)\n");
                s_dt_selftest_done = 1;
            }
        }
    }

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
        /* K.3: live pulse animation of the chain-graph overlay. */
        { extern int sigviz_overlay_visible(void); extern void sigviz_tick(void);
          if (sigviz_overlay_visible()) { sigviz_tick(); compositor_dirty_all(); } }
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

/* B.5/B.9: advance per-layer logic state (clock, hover, animation) WITHOUT
 * drawing. Kept separate from composite_draw() so the self-check can re-render
 * the identical state (draw-only) rather than advancing it a second time. */
static void composite_update(void)
{
    extern void panel_update(void);
    extern void dock_update(void);
    panel_update();
    dock_update();
}

/* B.5/B.9: the layer PAINT stack (no state advance), shared by the correction
 * pass and the self-check. Honors whatever fb clip the caller set (whole screen
 * if none). Deterministic bottom-to-top paint order. */
static void composite_draw(void)
{
    extern void desktop_draw(void);
    extern void panel_draw(void);
    extern void dock_draw(void);

    desktop_draw();       /* wallpaper + icons (bottom) */
    wm_draw_all();        /* windows */
    /* Panel stands down for a truly fullscreen surface: the user asked for one
     * thing on the screen, and "fullscreen" that still has a clock across it is
     * not fullscreen. Same authority the network indicator uses. */
    { extern int wm_fullscreen_active(void); if (!wm_fullscreen_active()) panel_draw(); }
    dock_draw();

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
        /* Command palette: topmost overlay. Was drawn only in the palette CHAIN
         * resolve (to the FRONT buffer), which the B.6/B.9 composite then flipped
         * over -> it opened but never showed. Draw it here in the composited
         * backbuffer like every other overlay. Self-gates on visibility. */
        { extern void palette_draw(void); palette_draw(); }
        { extern void sigviz_overlay_draw(void); sigviz_overlay_draw(); }  /* K.1 chain graph */
        { extern void popover_draw(void); popover_draw(); }  /* C.11 popover */
        { extern void sheet_draw(void); sheet_draw(); }  /* C.10 sheet */
        dirty_modal_draw();
    }
    theme_apply_night_shift();
    { extern void idle_post_overlay(void); idle_post_overlay(); }
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

#ifdef ZEOS_DIAG_B9_SELFCHECK
    /* Force a small region-delta each tick so the clip path + self-check
     * exercise deterministically on an idle desktop (region away from edges,
     * < 60% so use_clip engages). Validates that a clipped correction is
     * pixel-identical to a full redraw. */
    if (s_b9_frames < B9_SELFCHECK_FRAMES)
        compositor_dirty(200, 200, 240, 160);
#endif

    int dirty = compositor_consume_dirty();
#ifdef ZEOS_DIAG_KEYTRACE
    { extern void kputs(const char*); extern void kput_hex(uint64_t);
      extern int palette_is_visible(void);
      static uint32_t s_kt_present_n;
      s_kt_present_n++;
      /* Only trace every 30th idle present to avoid flooding; always trace when
       * dirty or palette visible so the interesting frames show. */
      if (dirty || palette_is_visible() || (s_kt_present_n % 30 == 0)) {
          kputs("[KT] present n="); kput_hex(s_kt_present_n);
          kputs(" dirty="); kput_hex((uint64_t)dirty);
          kputs(" pal_vis="); kput_hex((uint64_t)palette_is_visible());
          kputs("\n");
      }
    }
#endif
    if (dirty) {
        /* B.5/B.9 delta correction: clip the whole layer stack to the union of
         * the region-deltas pushed since the last composite. The B.6 back buffer
         * persists the last full scene, so redrawing only the delta region leaves
         * every other pixel correct for the bulk flip. Full redraw when a caller
         * asked for it (fully_dirty) or when the delta covers most of the screen
         * (clip bookkeeping would not pay off). */
        int use_clip = 0;
        int cx0 = 0, cy0 = 0, cx1 = (int)fb_width(), cy1 = (int)fb_height();
        if (!g_comp.fully_dirty && s_delta_valid) {
            cx0 = s_delta_x0; cy0 = s_delta_y0;
            cx1 = s_delta_x1; cy1 = s_delta_y1;
            if (cx0 < 0) cx0 = 0;
            if (cy0 < 0) cy0 = 0;
            if (cx1 > (int)fb_width())  cx1 = (int)fb_width();
            if (cy1 > (int)fb_height()) cy1 = (int)fb_height();
            long area = (long)(cx1 - cx0) * (long)(cy1 - cy0);
            long full = (long)fb_width() * (long)fb_height();
            if (area > 0 && area < (full * 3) / 5)   /* < 60% of screen: worth clipping */
                use_clip = 1;
        }

        composite_update();   /* advance layer logic once (clock/hover/anim) */

#ifdef ZEOS_DIAG_B9_SELFCHECK
        uint64_t t_clip0 = timer_read_tsc();
#endif
        fb_present_begin();   /* B.6: retarget writers to the WB back buffer */
        if (use_clip) {
            fb_set_clip(cx0, cy0, cx1 - cx0, cy1 - cy0);
            s_composite_clipped++;
        }
        composite_draw();
        if (use_clip) fb_reset_clip();   /* restore full-screen drawing */
        delta_reset();
        g_comp.fully_dirty = 0;
        if (use_clip)
            fb_present_end_rect(cx0, cy0, cx1 - cx0, cy1 - cy0);  /* partial flip */
        else
            fb_present_end();     /* B.6: full atomic flip */
#ifdef ZEOS_DIAG_B9_SELFCHECK
        uint64_t t_clip1 = timer_read_tsc();
#endif

#ifdef ZEOS_DIAG_B9_SELFCHECK
        /* The compositor watches its own output: the clipped correction just
         * produced F_partial. Force a full re-DRAW of the identical state (no
         * composite_update, so nothing advances) -> F_full. Matching checksums
         * prove the correction dropped nothing; a mismatch means partial redraw
         * left a stale pixel. Runs for the first B9_SELFCHECK_FRAMES composites. */
        if (use_clip && s_b9_frames < B9_SELFCHECK_FRAMES) {
            s_b9_clip_ticks += (t_clip1 - t_clip0);
            /* Compare BACK buffers (cursor-free): the clipped draw left the delta
             * region fresh + the rest from the last full draw; a full redraw
             * makes the whole backbuf fresh. Equal => the clipped correction
             * produced the same composed scene as a full redraw. */
            uint64_t h_partial = fb_backbuf_checksum();
            uint64_t t_full0 = timer_read_tsc();
            fb_present_begin();
            composite_draw();                 /* no clip, no update -> full redraw, same state */
            fb_present_end();
            uint64_t t_full1 = timer_read_tsc();
            uint64_t h_full = fb_backbuf_checksum();
            s_b9_full_ticks += (t_full1 - t_full0);
            s_b9_frames++;
            if (h_partial != h_full) {
                s_b9_mismatch++;
                kputs("[B9] MISMATCH frame="); kput_dec((uint64_t)s_b9_frames);
                kputs(" partial="); kput_hex(h_partial);
                kputs(" full=");    kput_hex(h_full); kputs("\n");
            }
            if (s_b9_frames == B9_SELFCHECK_FRAMES) {
                uint64_t freq = timer_tsc_freq();
                uint64_t clip_us = freq ? (s_b9_clip_ticks * 1000000ULL) / freq / s_b9_frames : 0;
                uint64_t full_us = freq ? (s_b9_full_ticks * 1000000ULL) / freq / s_b9_frames : 0;
                kputs("[B9] selfcheck: frames="); kput_dec((uint64_t)s_b9_frames);
                kputs(" clipped_total="); kput_dec((uint64_t)s_composite_clipped);
                kputs(" mismatches="); kput_dec((uint64_t)s_b9_mismatch);
                kputs(s_b9_mismatch == 0 ? " -> PASS" : " -> FAIL");
                kputs(" | avg clip="); kput_dec(clip_us);
                kputs("us full="); kput_dec(full_us);
                kputs("us (delta=240x160)\n");
            }
        }
#endif
    }

    /* Cursor: UNGATED, drawn on top every tick (it moves without a composite). */
    {
        extern int lockscreen_active(void);
        if (g_comp.layer_visible[COMP_LAYER_CURSOR] && !lockscreen_active())
            cursor_draw();
    }

    /* NETWORK ACTIVITY — the last thing drawn, above EVERYTHING, every tick.
     *
     * Not a desktop widget and not a dock item: a fullscreen program would hide
     * either one, and an indicator that can be covered is an indicator that can
     * be defeated. This is composited after all windows, after the panel, after
     * the cursor, with no layer_visible gate and no setting — the software
     * equivalent of a link LED on the chassis.
     *
     * It exists so that traffic nobody asked for is visible to the person using
     * the machine, immediately, without tools and without having to trust us. */
    { extern void netmon_draw_overlay(void); netmon_draw_overlay(); }
}

void compositor_dirty(int x, int y, int w, int h) {
    /* B.5/B.9: accumulate the region-delta instead of forcing a full redraw.
     * The correction pass clips to the union of these regions. Callers that
     * genuinely change the whole screen use compositor_dirty_all(). */
    delta_add(x, y, w, h);

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

static float s_l3_pos;
static void  l3_cb(int id, float v, void *c) { (void)id; (void)c; s_l3_pos = v; }
/* L.3 selftest: the compositor ticks anims every frame and re-arms compositing
 * while any spring is live. Spawn a spring, clear dirty, run ONE
 * compositor_advance(): the spring must have advanced (anim_tick was driven by
 * the compositor) AND dirty must be re-armed (anim_active_count>0 gate).
 * Un-gated so the production `selftest` shell can run it; PRODUCTION-SAFE: uses
 * anim_cancel on just its own probe spring instead of anim_init (which would
 * reset the whole live anim system). Returns 1 on PASS. */
int compositor_l3_selftest(void)
{
    s_l3_pos = 0.0f;
    int id = anim_spring(0.0f, 100.0f, SPRING_SNAPPY_S, SPRING_SNAPPY_D, l3_cb, 0);
    int live = (anim_active_count() > 0);
    (void)compositor_consume_dirty();          /* clear pending */
    compositor_advance();                       /* ticks anim + re-arms */
    int ticked  = (s_l3_pos > 0.0f);            /* compositor drove the spring */
    int rearmed = (compositor_consume_dirty() > 0);  /* re-armed while live */
    if (id >= 0) anim_cancel(id);               /* remove only our probe spring */
    int pass = live && ticked && rearmed;
    kputs("[L3] live="); kput_dec((uint64_t)live);
    kputs(" ticked(pos>0)="); kput_dec((uint64_t)ticked);
    kputs(" rearmed="); kput_dec((uint64_t)rearmed);
    kputs(pass ? " -> PASS\n" : " -> FAIL\n");
    return pass;
}
