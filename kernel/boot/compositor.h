/*
 * Zeos — Compositor
 *
 * Layered rendering pipeline:
 *   Layer 0: Desktop surface (wallpaper + icons)
 *   Layer 1: Chain surfaces (via window manager)
 *   Layer 2: Panel (top bar)
 *   Layer 3: Dock (bottom, auto-hide)
 *   Layer 4: Overlays (notifications, command palette, snap ghost)
 *   Layer 5: Cursor (always on top)
 *
 * Double-buffered: compose to back buffer, flip to front.
 * All rendering goes through the compositor — nothing draws
 * to the framebuffer directly during normal operation.
 */

#ifndef ZEOS_COMPOSITOR_H
#define ZEOS_COMPOSITOR_H

#include <stdint.h>

/* ── Compositor layers ── */
typedef enum {
    COMP_LAYER_DESKTOP,
    COMP_LAYER_SURFACES,
    COMP_LAYER_PANEL,
    COMP_LAYER_DOCK,
    COMP_LAYER_OVERLAYS,
    COMP_LAYER_CURSOR,
    COMP_LAYER_COUNT
} comp_layer_t;

/* ── Compositor state ── */
typedef struct {
    /* Screen dimensions */
    int screen_w, screen_h;

    /* Back buffer (compose here, then flip) */
    uint32_t *backbuf;
    int backbuf_size;

    /* Dirty region tracking (avoid full redraws) */
    int dirty_x, dirty_y, dirty_w, dirty_h;
    int fully_dirty;

    /* Layer visibility */
    int layer_visible[COMP_LAYER_COUNT];

    /* Panel */
    int panel_h;
    int panel_visible;

    /* Dock */
    int dock_h;
    int dock_visible;
    int dock_auto_hide;

    /* Desktop */
    uint32_t wallpaper_color;

    /* Frame timing */
    uint64_t last_frame_tsc;
    float    frame_dt;           /* Seconds since last frame */
    int      target_fps;
} compositor_t;

/* ── API ── */

/* Initialize the compositor (allocates back buffer) */
int compositor_init(int screen_w, int screen_h);

/* Compose a single frame: all layers → back buffer → flip to screen */
void compositor_frame(void);

/* Mark a region as needing redraw */
void compositor_dirty(int x, int y, int w, int h);

/* Mark entire screen dirty */
void compositor_dirty_all(void);

/* Drain pending dirty rects and return how many composite-worthy
 * regions were collected since the last call. Returns 0 if there is
 * nothing to redraw (composite can be skipped). */
int  compositor_consume_dirty(void);

/* Number of dirty rects pushed since boot (for tickrate / selftest). */
uint32_t compositor_dirty_pushes(void);

/* Number of composite passes the compositor actually ran (post skip). */
uint32_t compositor_composite_count(void);

/* Number of composite passes skipped due to no dirty rects. */
uint32_t compositor_composite_skips(void);

/* Set wallpaper color */
void compositor_set_wallpaper(uint32_t color);

/* Show/hide dock */
void compositor_show_dock(int show);

/* Get compositor state */
compositor_t *compositor_get_state(void);

#endif /* ZEOS_COMPOSITOR_H */
